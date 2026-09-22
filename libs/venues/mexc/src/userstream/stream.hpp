#pragma once

// The private socket, and only that.
//
// It logs in and turns each personal push into an event on the sink. It sends
// no orders: an order goes out over REST and is ANSWERED here, and joining
// those two halves is the execution client's job, not this one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <string>

#include "../state.hpp"

namespace raikiri::venues::mexc::internal {

constexpr std::uint32_t k_private_stream_id = 1;

// Routes one futures personal push. MEXC sends these unbidden after a login,
// so the socket carrying them is authenticated, not subscribed.
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
        // Remembered BEFORE the event is emitted: a deal for the same fill may
        // already be queued behind this frame, and it can only state its
        // leaves out of this memo.
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
        // GatewaySink carries no position event; the reconciler asks for
        // positions through position_reports() instead.
        break;
    case EventKind::LoginAck:
        state.sink->on_connectivity({.state = exec::ConnState::Connected,
                                     .stream_id = k_private_stream_id,
                                     .ts = state.clock->wall()});
        break;
    case EventKind::Error: {
        // rs.error carries prose and no code at all, so a rejected login and a
        // rejected subscription are indistinguishable here; the socket is only
        // ever used for a login, which is what makes AuthFailed the reading.
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

// The futures private socket. It is a SECOND connection to the same /edge
// endpoint the public one uses: MEXC authenticates the socket rather than
// minting a listen key, and the personal pushes follow the login unbidden.
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

// The socket itself, owned as one thing so a caller cannot hold the handler
// without the client it is attached to.
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

    // MEXC's 60 s idle timer counts only CLIENT messages, so the data client's
    // ping tick has to reach this socket too — it is the one timer this
    // adapter has.
    [[nodiscard]] net::WsClient& client() noexcept { return ws_; }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

} // namespace raikiri::venues::mexc::internal
