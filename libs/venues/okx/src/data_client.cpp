#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::okx::detail {

namespace {

// OKX drops a connection with no application message for 30 s and does not
// count websocket control frames toward that timer, so the app ping runs well
// inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::Bbo: {
        const auto wire = parse_bbo(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::FundingRate: {
        const auto wire = parse_funding_rate(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Tickers: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Books: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->inst_id);
        if (instrument == nullptr) {
            return;
        }
        auto& book = state.book_for(*instrument);
        const auto result = book.apply(*wire);
        if (result == BookResult::GapDetected || result == BookResult::BadLevel) {
            return; // the venue resends a snapshot; until then we publish nothing
        }
        if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
            depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarkPrice:
    case EventKind::SubscribeAck:
    case EventKind::LoginAck:
    case EventKind::Orders:
    case EventKind::Unknown:
        break; // control frames, and the private topics of the other socket
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.channel = entry.channel, .inst_id = entry.inst_id});
        }
        std::size_t sent = 0;
        while (sent < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(sent));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
            sent += frame->consumed;
        }
        // Whatever the seqId chain said before the socket dropped says nothing
        // about the snapshot OKX is about to send.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class OkxData final : public exec::DataClient {
public:
    explicit OkxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        open(channel, symbol);
        // The turnover has no SubKind of its own and does not ride the
        // funding-rate frame the way it does on bybit and gate, so it is
        // opened beside it — otherwise nothing ever subscribes `tickers` and
        // every OKX row on the board keeps an empty volume cell.
        if (kind == exec::SubKind::Funding) {
            open(k_tickers_channel, symbol);
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        if (client_.connected() && now.ns >= state_->next_app_ping_ns) {
            (void)client_.send(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    // Records the subscription and, on a live socket, sends it now; a socket
    // still connecting picks it up from `desired` in on_open.
    void open(std::string_view channel, std::string_view symbol) {
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return;
        }
        state_->add_subscription(channel, symbol);
        if (!client_.connected()) {
            return;
        }
        const std::array<Subscription, 1> one{Subscription{.channel = channel, .inst_id = symbol}};
        if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
            (void)client_.send(frame->text);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<OkxData>(std::move(state));
}

} // namespace raikiri::venues::okx::detail
