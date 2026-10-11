#pragma once

#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/orders.hpp>
#include <raikiri/venues/aster/ws.hpp>

#include <string>
#include <string_view>

#include "../state.hpp"

namespace raikiri::venues::aster::internal {

// kept out of the client so dispatch is testable without a socket
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    // each drop loses an order update; the first of each kind is logged, then only counted
    const auto drop = [&state](std::string_view why) {
        if (state.private_frames_dropped == 0) {
            RLOG_WARN("aster: dropped a private frame — {}; the book will not see it", why);
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
        if (const auto order_event = map_order_event(*wire, state.instruments, received); order_event.has_value()) {
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

// the key is in the target, so a new key means a new stream object
class PrivateStream {
public:
    PrivateStream(VenueState& state, std::string_view key)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{ep::stream_host(state.testnet)},
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

} // namespace raikiri::venues::aster::internal
