#pragma once

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/orders.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::htx::internal {

constexpr std::uint32_t k_private_stream_id = 1;

// spot v2 is plain text; payloads without gzip magic pass through
std::optional<std::string_view>
decode_frame(net::Inflater& inflater, std::string_view payload, std::uint64_t& failures) {
    const auto inflated = inflater.inflate(payload);
    if (inflated.has_value()) {
        return *inflated;
    }
    if (inflated.error() == net::InflateError::NotGzip) {
        return payload;
    }
    ++failures;
    return std::nullopt;
}

void publish_order(VenueState& state, const WireOrder& wire, UnixNanos received) {
    const auto leaves = order_leaves(wire, state.instruments);
    for (const auto& fill : wire.fills) {
        if (const auto event = map_fill(
                wire, fill, state.instruments, leaves.value_or(QtyLots::from_raw(0)), received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
    if (const auto event = map_order_event(wire, state.instruments, received); event.has_value()) {
        state.sink->on_order_event(*event);
    }
    state.remember(wire);
}

// same endpoint as the funding stream, but signed, so it gets its own connection
class SwapPrivateHandler final : public net::WsHandler {
public:
    explicit SwapPrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto frame = state_->protocol.swap_auth_frame(state_->signer, state_->venue_now());
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = decode_frame(inflater_, payload, state_->inflate_failures);
            if (!frame.has_value()) {
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return;
            }
            switch (event->kind) {
            case EventKind::NotifyPing:
                if (const auto pong = state_->protocol.notify_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
                return;
            case EventKind::AuthReply:
                on_auth(*frame);
                return;
            case EventKind::PrivateOrder: {
                static thread_local const auto orders_owner = std::make_unique<OrderBuffer>();
                auto& orders = *orders_owner;
                if (const auto wire = parse_swap_order_push(*frame, orders); wire.has_value()) {
                    publish_order(*state_, *wire, received);
                }
                return;
            }
            default:
                return; // acks and position pushes carry nothing we act on
            }
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void on_auth(std::string_view frame) {
        const auto reply = parse_auth_reply(frame);
        if (!reply.has_value() || !reply->ok) {
            state_->last_error_code = reply.has_value() ? std::string{reply->code} : std::string{};
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        // matchOrders_cross is not subscribed: it repeats orders_cross trades without the fee
        const auto frames = std::array<std::string_view, 1>{k_swap_orders_topic};
        for (const auto& topic : frames) {
            const auto sub = state_->protocol.swap_private_subscribe_frame(topic);
            if (!sub.has_value() || !client_->send(sub->text)) {
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = k_private_stream_id,
                                               .ts = state_->clock->wall()});
                return;
            }
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

// plain text; orders carry leaves, and trade.clearing carries the fee and exec id
class SpotPrivateHandler final : public net::WsHandler {
public:
    explicit SpotPrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto frame = state_->protocol.spot_auth_frame(state_->signer, state_->venue_now());
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = decode_frame(inflater_, payload, state_->inflate_failures);
            if (!frame.has_value()) {
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return;
            }
            switch (event->kind) {
            case EventKind::SpotPing:
                if (const auto pong = state_->protocol.spot_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
                return;
            case EventKind::AuthReply:
                on_auth(*frame);
                return;
            case EventKind::PrivateOrder:
                if (const auto wire = parse_spot_order_push(*frame); wire.has_value()) {
                    publish_order(*state_, *wire, received);
                }
                return;
            case EventKind::PrivateMatch:
                on_trade(*frame, received);
                return;
            default:
                return;
            }
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void on_auth(std::string_view frame) {
        const auto reply = parse_auth_reply(frame);
        if (!reply.has_value() || !reply->ok) {
            state_->last_error_code = reply.has_value() ? std::string{reply->code} : std::string{};
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        const auto channels =
            std::array<std::string_view, 2>{k_spot_orders_channel, k_spot_trades_channel};
        for (const auto& channel : channels) {
            const auto sub = state_->protocol.spot_private_subscribe_frame(channel);
            if (!sub.has_value() || !client_->send(sub->text)) {
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = k_private_stream_id,
                                               .ts = state_->clock->wall()});
                return;
            }
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_trade(std::string_view frame, UnixNanos received) {
        const auto wire = parse_spot_trade_push(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto memo = state_->memo_for(wire->order_id);
        if (const auto event = map_user_trade(*wire,
                                              state_->instruments,
                                              memo.leaves,
                                              memo.client_order_id,
                                              memo.exchange_client_order_id,
                                              received);
            event.has_value()) {
            state_->sink->on_order_event(*event);
        }
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

template <typename Handler> class PrivateStream {
public:
    PrivateStream(VenueState& state, std::string_view host, std::string_view path)
        : handler_{state}, client_{state.io,
                                   net::WsConfig{.host = std::string{host},
                                                 .port = std::string{stream_port()},
                                                 .target = std::string{path}},
                                   handler_} {
        handler_.attach(client_);
    }

    void start() { client_.start(); }

    void stop() { client_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return client_.connected(); }

private:
    Handler handler_;
    net::WsClient client_;
};

} // namespace raikiri::venues::htx::internal
