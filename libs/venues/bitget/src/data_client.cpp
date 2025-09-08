#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitget::detail {

namespace {

// Bitget drops a connection idle for 30 s and does not count websocket
// control frames toward that timer, so the app ping runs inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
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
    case EventKind::Books: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        // Terminal for the channel it names, and silence is how it would otherwise present.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::Orders:
    case EventKind::LoginAck:
        break; // the private socket's, and it never shares this handler
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path, which is why the transport tracks none.
        const auto type = inst_type(state_->market, state_->testnet);
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back(
                {.inst_type = type, .channel = entry.channel, .inst_id = entry.inst_id});
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

class BitgetData final : public exec::DataClient {
public:
    explicit BitgetData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host()},
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
        const bool added = state_->add_subscription(channel, symbol);
        if (added && client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.inst_type = inst_type(state_->market, state_->testnet),
                             .channel = channel,
                             .inst_id = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
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
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitgetData>(std::move(state));
}

} // namespace raikiri::venues::bitget::detail
