#pragma once

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/orders.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <string>

#include "../state.hpp"

namespace raikiri::venues::bingx::internal {

constexpr std::uint32_t k_perp_stream_id = 1;

constexpr std::uint32_t k_spot_stream_id = 2;

// only the spot user stream needs a subscribe
constexpr std::string_view k_spot_user_channel = "spot.executionReport";

// market comes from the socket; pushes do not say which tree
void handle_private_frame(VenueState& state,
                          MarketType market,
                          std::string_view frame,
                          UnixNanos received) {
    const auto kind = parse_private_event(frame);
    if (!kind.has_value()) {
        return;
    }
    const auto stream_id = market == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
    switch (*kind) {
    case PrivateKind::OrderUpdate: {
        const auto wire = parse_order_update(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto fallback = state.remembered(wire->order_id);
        const auto event = map_order_update(*wire, state.instruments, market, fallback, received);
        if (!event.has_value()) {
            return;
        }
        if (!event->exchange_client_order_id.empty()) {
            state.remember(wire->order_id, event->exchange_client_order_id);
        }
        state.sink->on_order_event(*event);
        // drop the memo entry once the order is closed
        if (event->type == OrderEventType::Canceled || event->type == OrderEventType::Expired ||
            (event->type == OrderEventType::Filled && event->leaves_qty.raw == 0)) {
            state.forget(wire->order_id);
        }
        return;
    }
    case PrivateKind::ListenKeyExpired:
        state.key_stale = true;
        state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateKind::Error:
        if (const auto control = parse_control(frame); control.has_value()) {
            state.last_error_code = control->code;
        }
        state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateKind::Ack:
        state.sink->on_connectivity({.state = exec::ConnState::Connected,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateKind::Ignorable:
    case PrivateKind::Unknown:
        return;
    }
}

// same gzip framing and ping as the public socket; auth is the listen key in the url
class PrivateHandler final : public net::WsHandler {
public:
    PrivateHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        const auto stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
        if (market_ == MarketType::Spot && client_ != nullptr) {
            const auto frame = protocol_.subscribe_channel(k_spot_user_channel);
            if (!frame.has_value() || !client_->send(frame->text)) {
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = stream_id,
                                               .ts = state_->clock->wall()});
                return;
            }
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = inflater_.inflate(payload);
            if (!frame.has_value()) {
                ++state_->inflate_failures;
                return;
            }
            if (is_ping(*frame)) {
                if (client_ != nullptr) {
                    (void)client_->send(Protocol::pong_frame());
                }
                return;
            }
            handle_private_frame(*state_, market_, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected,
             .stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id,
             .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    MarketType market_;
    Protocol protocol_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

// the key is baked into the target, so a new key needs a new stream
class PrivateStream {
public:
    PrivateStream(VenueState& state, MarketType market, const std::string& key)
        : handler_{state, market},
          ws_{state.io,
              net::WsConfig{.host = std::string{private_stream_host(market)},
                            .port = "443",
                            .target = private_stream_target(market, key)},
              handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

} // namespace raikiri::venues::bingx::internal
