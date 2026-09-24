#pragma once

// logs in and turns personal pushes into sink events; the execution client joins them to orders

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <string>

#include "../state.hpp"

namespace raikiri::venues::mexc::internal {

constexpr std::uint32_t k_private_stream_id = 1;

// mexc pushes these unbidden after login
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::PersonalOrder: {
        const auto wire = parse_futures_order(frame);
        if (!wire.has_value()) {
            return;
        }
        // remembered before emitting so a queued deal can read its leaves
        state.remember(*wire);
        if (const auto mapped = map_futures_order_event(*wire, state.instruments, received);
            mapped.has_value()) {
            state.sink->on_order_event(*mapped);
        }
        break;
    }
    case EventKind::PersonalDeal: {
        const auto wire = parse_futures_deal(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto memo = state.memo_for(wire->order_id);
        if (const auto mapped = map_futures_deal(
                *wire, state.instruments, memo.leaves, memo.client_order_id, received);
            mapped.has_value()) {
            state.sink->on_order_event(*mapped);
        }
        break;
    }
    case EventKind::PersonalPosition:
        // no position event on the sink; the reconciler polls position_reports()
        break;
    case EventKind::LoginAck:
        state.sink->on_connectivity({.state = exec::ConnState::Connected,
                                     .stream_id = k_private_stream_id,
                                     .ts = state.clock->wall()});
        break;
    case EventKind::Error: {
        // rs.error has no code; this socket only logs in, so treat it as an auth failure
        if (parse_stream_error(frame).has_value()) {
            state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                         .stream_id = k_private_stream_id,
                                         .ts = state.clock->wall()});
        }
        break;
    }
    case EventKind::Ticker:
    case EventKind::Trade:
    case EventKind::Depth:
    case EventKind::DepthFull:
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

// a second connection to /edge, authenticated by login
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto frame =
            state_->protocol.login_frame(state_->signer, state_->venue_now(MarketType::Perp));
        if (!frame.has_value() || client_ == nullptr || !client_->send(*frame)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{stream_host(state.market)},
                                             .port = std::string{stream_port()},
                                             .target = std::string{stream_path(state.market)}},
                               handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

    // the data client's ping tick reaches this socket; the idle timer
    // counts only client messages
    [[nodiscard]] net::WsClient& client() noexcept { return ws_; }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

} // namespace raikiri::venues::mexc::internal
