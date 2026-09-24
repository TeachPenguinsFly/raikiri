#pragma once

// every private frame is a positional array; the tag in slot 1 says what it is

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <array>
#include <memory>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::bitfinex::internal {

constexpr std::uint32_t k_private_stream_id = 1;

// everything private rides channel 0; tags with no sink channel are dropped unparsed
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) noexcept {
    const auto info = parse_frame(frame);
    if (!info.has_value()) {
        return;
    }
    if (info->kind == FrameKind::Auth) {
        const auto ack = parse_auth_ack(frame);
        const bool ok = ack.has_value() && ack->ok;
        if (!ok && ack.has_value()) {
            state.last_error_code = ack->code;
        }
        state.sink->on_connectivity(
            {.state = ok ? exec::ConnState::Connected : exec::ConnState::AuthFailed,
             .stream_id = k_private_stream_id,
             .ts = state.clock->wall()});
        return;
    }
    if (info->kind != FrameKind::Data || info->chan_id != 0) {
        return;
    }

    const auto remember = [&state](const WireOrder& order) {
        const auto client_id = state.client_id_for(order);
        auto context = OrderContext{.client_order_id = client_id, .leaves = QtyLots::from_raw(0)};
        if (const auto report = map_open_order(order, state.instruments); report.has_value()) {
            context.leaves = report->leaves;
        }
        state.remember(order.id, context);
        return client_id;
    };

    if (info->tag == "on" || info->tag == "ou" || info->tag == "oc") {
        const auto wire = parse_order(frame);
        if (!wire.has_value()) {
            return;
        }
        // remembered before emitting: an execution for this order may follow immediately
        const auto client_id = remember(*wire);
        if (const auto event = map_order_event(*wire, state.instruments, client_id, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    if (info->tag == "os") {
        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
        auto& storage = *storage_owner;
        const auto orders = parse_orders(frame, storage);
        if (!orders.has_value()) {
            return;
        }
        // a snapshot seeds the memo only; it emits no transition
        for (const auto& wire : *orders) {
            (void)remember(wire);
        }
        return;
    }
    if (info->tag == "tu") {
        // te and tu are the same fill; only tu, which carries the fee, is emitted
        const auto wire = parse_execution(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto event = map_execution(
                *wire, state.instruments, state.context_for(wire->order_id), received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    // te, positions and n are ignored: positions are pulled over rest and writes read their own answer
}

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::array<char, 512> buffer{};
        const auto frame = ws_auth_frame(state_->api_key,
                                         state_->api_secret.view(),
                                         nonces_.next(state_->clock->wall().ns),
                                         buffer);
        if (frame.empty() || client_ == nullptr || !client_->send(frame)) {
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
    NonceSource nonces_;
};

class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{stream_host()},
                                             .port = std::string{stream_port()},
                                             .target = std::string{stream_path()}},
                               handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

    // with no public subscriptions there are no heartbeats, so it pings to stay open
    void heartbeat(UnixNanos now) noexcept {
        if (now.ns < next_ping_ns_) {
            return;
        }
        if (ws_.connected()) {
            (void)ws_.send(Protocol::ping_frame());
        }
        next_ping_ns_ = now.ns + k_app_ping_interval_ns;
    }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
    std::uint64_t next_ping_ns_{0};
};

} // namespace raikiri::venues::bitfinex::internal
