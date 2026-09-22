#pragma once

// The private socket, and only that.
//
// It authenticates, subscribes and turns each frame into an event on the sink.
// It sends no orders: an order goes out over REST and is ANSWERED here, and
// joining those two halves is the execution client's job, not this one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/endpoints.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <memory>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::okx::internal {

void handle_order_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
    auto& storage = *storage_owner;
    const auto orders = parse_orders(frame, storage);
    if (!orders.has_value()) {
        return;
    }
    for (const auto& wire : *orders) {
        // The fill goes first. One message is both the last trade and the
        // cancel that ended the order, and publishing the terminal event
        // ahead of the money moves an order to Canceled with a fill still
        // unaccounted for.
        if (const auto fill = map_order_fill(wire, state.instruments, received); fill.has_value()) {
            state.sink->on_order_event(*fill);
        }
        if (const auto event = map_order_event(wire, state.instruments, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
}

// Private order socket: login first, subscribe only once OKX answers it.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto login = state_->private_protocol.login_frame(
            state_->signer, state_->passphrase.view(), state_->venue_now());
        if (!login.has_value() || !client_->send(*login)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            return;
        }
        // No subscribe here on purpose: a private channel asked for before the
        // login answer lands is refused with 60011, and the refusal looks
        // exactly like a channel that has nothing to say.
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return; // the bare "pong", and anything OKX adds later
        }
        switch (event->kind) {
        case EventKind::LoginAck: {
            const auto ack = parse_stream_error(payload);
            const bool authenticated = ack.has_value() && ack->code == k_ok;
            if (!authenticated) {
                state_->last_error_code = ack.has_value() ? ack->code : 0;
            }
            state_->sink->on_connectivity({.state = authenticated ? exec::ConnState::Resubscribed
                                                                  : exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            if (authenticated) {
                subscribe_orders();
            }
            break;
        }
        case EventKind::Orders:
            handle_order_frame(*state_, payload, received);
            break;
        case EventKind::Error:
            if (const auto err = parse_stream_error(payload); err.has_value()) {
                state_->last_error_code = err->code;
                ++state_->subscribe_refusals;
            }
            break;
        case EventKind::Bbo:
        case EventKind::Trade:
        case EventKind::FundingRate:
        case EventKind::MarkPrice:
        case EventKind::Tickers:
        case EventKind::Books:
        case EventKind::SubscribeAck:
        case EventKind::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void subscribe_orders() {
        // OKX offers a dedicated `fills` channel, but it answers 64003 below
        // trading fee tier VIP4 and its own docs send everyone else here: the
        // orders channel carries fillPx/fillSz/tradeId on every trade, so it
        // is the fill feed as well as the lifecycle feed.
        std::array<Subscription, 2> args{};
        std::size_t count = 0;
        if (state_->spot_enabled) {
            args[count++] = {.channel = k_orders_channel,
                             .inst_type = inst_type_of(MarketType::Spot)};
        }
        if (state_->perp_enabled) {
            args[count++] = {.channel = k_orders_channel,
                             .inst_type = inst_type_of(MarketType::Perp)};
        }
        if (count == 0) {
            return;
        }
        const auto frame = state_->private_protocol.subscribe_frame(
            std::span<const Subscription>{args.data(), count});
        if (frame.has_value()) {
            (void)client_->send(frame->text);
        }
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
};

// The socket itself, owned as one thing so a caller cannot hold the handler
// without the client it is attached to.
class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{stream_host(state.testnet)},
                                             .port = std::string{stream_port()},
                                             .target = std::string{private_stream_path()}},
                               handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

} // namespace raikiri::venues::okx::internal
