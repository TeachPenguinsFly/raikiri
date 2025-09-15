#pragma once

// The private socket, and only that.
//
// It logs in, subscribes and turns each frame into an event on the sink. It
// sends no orders: an order goes out over REST and is ANSWERED here, and
// joining those two halves is the execution client's job, not this one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/orders.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::bitget::detail {

// Private order stream. Login FIRST and subscribe only once Bitget answers:
// a subscribe sent before the login lands is refused with 30004 and never
// retried, leaving a socket that is up, silent, and indistinguishable from a
// quiet account.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto login = state_->protocol.login_frame(
            state_->signer, state_->passphrase.view(), state_->venue_now());
        if (!login.has_value() || !client_->send(*login)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return; // the bare "pong", and whatever Bitget adds next
        }
        switch (event->kind) {
        case EventKind::LoginAck:
            subscribe_orders();
            state_->sink->on_connectivity(
                {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
            break;
        case EventKind::Orders: {
            static thread_local OrderUpdateBuffer storage;
            const auto updates = parse_order_updates(payload, storage);
            if (!updates.has_value()) {
                return;
            }
            for (const auto& wire : *updates) {
                if (const auto mapped = map_order_update(wire, state_->instruments, received);
                    mapped.has_value()) {
                    state_->sink->on_order_event(*mapped);
                }
            }
            break;
        }
        case EventKind::Error: {
            const auto err = parse_stream_error(payload);
            if (!err.has_value()) {
                return;
            }
            state_->last_error_code = err->code;
            if (is_login_failure(err->code)) {
                // Bitget closes the socket when a login fails, so the
                // reconnect will fail identically until a human intervenes.
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = 1,
                                               .ts = state_->clock->wall()});
            } else {
                ++state_->subscribe_refusals;
            }
            break;
        }
        case EventKind::SubscribeAck:
        case EventKind::Ticker:
        case EventKind::Trade:
        case EventKind::Books:
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
    void subscribe_orders() noexcept {
        const std::array<Subscription, 1> orders{
            Subscription{.inst_type = inst_type(state_->market, state_->testnet),
                         .channel = k_channel_orders,
                         .inst_id = k_all_instruments}};
        if (const auto frame = state_->protocol.subscribe_frame(orders); frame.has_value()) {
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
                               net::WsConfig{.host = std::string{stream_host()},
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

} // namespace raikiri::venues::bitget::detail
