#pragma once

// The private socket, and only that.
//
// It turns each user-stream frame into an event on the sink. It sends no
// orders: an order goes out over REST and is ANSWERED here, and joining those
// two halves is the execution client's job, not this one's. It does not mint
// the listen key either — the key rides the URL and REST is the only place to
// get one, so the joint hands it in already made.

#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/orders.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <string>
#include <string_view>

#include "../state.hpp"

namespace raikiri::venues::binance::internal {

// Handles one user-stream frame. Kept out of the client so dispatch is
// testable without a socket.
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    // SAID, NOT SWALLOWED. Every `return` below drops an order update, and an
    // order update is how a fill reaches the book — lose one and the position
    // book disagrees with the venue until reconciliation halts the kernel,
    // which is a long way from the frame that caused it. The first of each kind
    // is logged; after that only the count grows, because a malformed stream
    // must not become the log.
    const auto drop = [&state](std::string_view why) {
        if (state.private_frames_dropped == 0) {
            RLOG_WARN("binance: dropped a private frame — {}; the book will not see it", why);
        }
        ++state.private_frames_dropped;
    };
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        drop("unreadable envelope");
        return;
    }
    switch (event->kind) {
    case EventKind::OrderUpdate: {
        const auto wire = parse_order_update(frame);
        if (!wire.has_value()) {
            drop("an ORDER_TRADE_UPDATE whose body would not parse");
            return;
        }
        if (const auto order_event = map_order_event(*wire, state.instruments, received);
            order_event.has_value()) {
            state.sink->on_order_event(*order_event);
        } else {
            drop("an order update naming an instrument this catalogue does not hold");
        }
        break;
    }
    case EventKind::ListenKeyExpired:
        state.listen_key_expired = true;
        break;
    case EventKind::AccountUpdate:
        break;
    default:
        break;
    }
}

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
};

// The socket itself, owned as one thing so a caller cannot hold the handler
// without the client that reports to it. The key is baked into the target, so
// a fresh key means a fresh stream object rather than a fresh subscribe frame.
class PrivateStream {
public:
    PrivateStream(VenueState& state, std::string_view key)
        : handler_{state},
          ws_{state.io,
              net::WsConfig{.host = std::string{stream_host(state.testnet)},
                            .port = "443",
                            .target = std::string{ep::k_ws_user_prefix} + std::string{key}},
              handler_} {}

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

} // namespace raikiri::venues::binance::internal
