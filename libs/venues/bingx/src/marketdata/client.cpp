#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bingx::detail {

namespace {

// Funding is a poll, not a stream (see wire.hpp). One symbol goes out per tick,
// so the request rate is bounded by the cadence rather than by how many
// contracts the gateway watches.
constexpr std::uint64_t k_funding_poll_interval_ns = 2'000'000'000ULL;

void handle_frame(VenueState& state, EventKind kind, std::string_view frame, UnixNanos received) {
    switch (kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, state.market, received);
            bbo.has_value()) {
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
            if (const auto trade = map_trade(entry, state.instruments, state.market, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, state.market, received);
            book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, state.market, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto control = parse_control(frame); control.has_value()) {
            state.last_error_code = control->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarkPrice:
    case EventKind::Ping:
    case EventKind::ControlAck:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& entry : state_->desired) {
            const auto frame = state_->protocol.subscribe_frame(entry.symbol, entry.suffix);
            if (!frame.has_value()) {
                continue;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = inflater_.inflate(payload);
            if (!frame.has_value()) {
                ++state_->inflate_failures;
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return; // unknown shapes are ignored, never fatal
            }
            if (event->kind == EventKind::Ping) {
                (void)client_->send(Protocol::pong_frame());
                return;
            }
            handle_frame(*state_, event->kind, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

class BingxData final : public exec::DataClient {
public:
    explicit BingxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->market)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path(state_->market)}},
                  handler_},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        client_.start();
        rest_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // This socket is one deployment's, and BingX spells a spot pair
            // and its perpetual identically — an id from the other market
            // subscribed here would be fed the wrong tree's prints under the
            // right-looking symbol, which is worse than no data.
            return std::unexpected{exec::not_implemented("bingx: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            // Not a stream: BingX publishes funding over REST only. The
            // 24-hour window IS a stream, and this is the only kind a caller
            // asks the venue's own statistics with, so the poll and the
            // ticker subscription are registered together.
            subscribe_channel(symbol, k_ticker_channel);
            for (const auto& existing : state_->funding_symbols) {
                if (existing == symbol) {
                    return {};
                }
            }
            state_->funding_symbols.emplace_back(symbol);
            return {};
        }
        const auto suffix = data_type_suffix(kind, state_->market);
        if (suffix.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        subscribe_channel(symbol, suffix);
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        poll_funding(now);
        // One io context serves both clients, and this is the exec client's
        // only tick.
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    void subscribe_channel(std::string_view symbol, std::string_view suffix) {
        state_->add_subscription(symbol, suffix);
        if (client_.connected()) {
            if (const auto frame = state_->protocol.subscribe_frame(symbol, suffix);
                frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
    }

    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_symbols.empty() || now.ns < state_->next_funding_poll_ns ||
            funding_in_flight_) {
            return;
        }
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
        const auto& symbol = state_->funding_symbols[state_->funding_cursor];
        state_->funding_cursor = (state_->funding_cursor + 1) % state_->funding_symbols.size();
        funding_in_flight_ = true;
        try {
            if (!rest_.send(builder_.premium_index(symbol),
                            [this](const net::RestResponse& response) { on_funding(response); })) {
                funding_in_flight_ = false;
            }
        } catch (...) {
            funding_in_flight_ = false;
        }
    }

    void on_funding(const net::RestResponse& response) noexcept {
        funding_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return; // the next tick retries; a missed rate is not an incident
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value() || envelope->code != k_ok) {
            return;
        }
        const auto wire = parse_premium_index(response.body);
        if (!wire.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        if (const auto funding = map_funding(*wire, state_->instruments, received);
            funding.has_value()) {
            state_->sink->on_funding(*funding);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    net::RestClient rest_;
    RequestBuilder builder_;
    bool funding_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BingxData>(std::move(state));
}

} // namespace raikiri::venues::bingx::detail
