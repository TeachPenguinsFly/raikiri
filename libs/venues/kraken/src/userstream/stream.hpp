#pragma once

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/orders.hpp>
#include <raikiri/venues/kraken/sign.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::kraken::internal {

// the private socket reports its own health
constexpr std::uint32_t k_private_stream_id = 1;

OrderEventType event_type_of(DeltaEffect effect) noexcept {
    switch (effect) {
    case DeltaEffect::Triggered:
        return OrderEventType::Triggered;
    case DeltaEffect::Canceled:
        return OrderEventType::Canceled;
    case DeltaEffect::Rejected:
        return OrderEventType::Rejected;
    case DeltaEffect::Accepted:
    case DeltaEffect::Silent:
        break;
    }
    // silent frames update the memo; the type is never published
    return OrderEventType::Accepted;
}

LiveOrder memo_of(const OrderEvent& event) noexcept {
    return {.instrument = event.instrument,
            .client_order_id = event.exchange_client_order_id,
            .side = event.side,
            .order_type = event.order_type,
            .flags = event.flags,
            .price = event.price,
            .qty = event.qty,
            .leaves = event.leaves_qty};
}

// a second WsClient: the signed challenge is valid for one connection
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    void on_open() override {
        // the challenge is per connection
        challenge_len_ = 0;
        signed_len_ = 0;
        const auto frame = protocol_.challenge_request_frame(state_->signer.api_key());
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return;
        }
        switch (event->kind) {
        case EventKind::Challenge:
            on_challenge(payload);
            return;
        case EventKind::OpenOrdersSnapshot:
            on_snapshot(payload, received);
            return;
        case EventKind::OpenOrders:
            on_delta(payload, received);
            return;
        case EventKind::Fills:
            on_fills(payload, received);
            return;
        case EventKind::Alert:
            // a refused private subscribe arrives as an alert, the same shape as a bad product id
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
            return;
        case EventKind::Subscribed:
        case EventKind::Info:
        case EventKind::Ticker:
        case EventKind::TickerLite:
        case EventKind::Trade:
        case EventKind::TradeSnapshot:
        case EventKind::Book:
        case EventKind::Unknown:
            // public feeds are listed so a new feed is a compile error
            return;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
    }

private:
    void on_challenge(std::string_view frame) noexcept {
        const auto wire = parse_challenge(frame);
        if (!wire.has_value() || wire->message.empty() || wire->message.size() > challenge_.size()) {
            return;
        }
        std::memcpy(challenge_.data(), wire->message.data(), wire->message.size());
        challenge_len_ = wire->message.size();
        const auto signature = authent(
            state_->signer.secret().view(), std::string_view{challenge_.data(), challenge_len_}, signed_challenge_);
        if (signature.empty()) {
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
            return;
        }
        signed_len_ = signature.size();
        if (!subscribe(k_feed_open_orders) || !subscribe(k_feed_fills)) {
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
    }

    [[nodiscard]] bool subscribe(std::string_view feed) noexcept {
        const auto frame = protocol_.private_subscribe_frame(feed,
                                                             state_->signer.api_key(),
                                                             {challenge_.data(), challenge_len_},
                                                             {signed_challenge_.data(), signed_len_});
        return frame.has_value() && client_->send(frame->text);
    }

    // the snapshot is remembered, not published; reconciliation settles gaps
    void on_snapshot(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage_owner = std::make_unique<OrderSnapshotBuffer>();
        auto& storage = *storage_owner;
        const auto orders = parse_order_snapshot(frame, storage);
        if (!orders.has_value()) {
            return;
        }
        for (const auto& wire : *orders) {
            if (const auto event = map_ws_order(wire, OrderEventType::Accepted, state_->instruments, received);
                event.has_value()) {
                state_->remember(wire.order_id, memo_of(*event));
            }
        }
    }

    void on_delta(std::string_view frame, UnixNanos received) noexcept {
        const auto delta = parse_order_delta(frame);
        if (!delta.has_value()) {
            return;
        }
        const auto effect = effect_of(delta->reason, delta->is_cancel);
        if (delta->has_order) {
            const auto event = map_ws_order(delta->order, event_type_of(effect), state_->instruments, received);
            if (!event.has_value()) {
                return;
            }
            state_->remember(delta->order.order_id, memo_of(*event));
            if (effect != DeltaEffect::Silent) {
                state_->sink->on_order_event(*event);
            }
            return;
        }
        // a removal names only an order id; the memo supplies the instrument
        const auto* memo = state_->recall(delta->order_id);
        if (memo == nullptr) {
            return;
        }
        if (effect != DeltaEffect::Silent) {
            OrderEvent event{};
            event.type = event_type_of(effect);
            event.instrument = memo->instrument;
            event.side = memo->side;
            event.order_type = memo->order_type;
            event.flags = memo->flags;
            event.exchange_client_order_id = memo->client_order_id;
            if (const auto id = ExchangeOrderId::from(delta->order_id); id.has_value()) {
                event.exchange_order_id = *id;
            }
            event.price = memo->price;
            event.qty = memo->qty;
            event.leaves_qty = QtyLots::from_raw(0);
            event.ts_event = received;
            event.ts_init = received;
            state_->sink->on_order_event(event);
        }
        state_->live_orders.erase(std::string{delta->order_id});
    }

    void on_fills(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage_owner = std::make_unique<WsFillBuffer>();
        auto& storage = *storage_owner;
        const auto fills = parse_ws_fills(frame, storage);
        if (!fills.has_value()) {
            return;
        }
        for (const auto& wire : *fills) {
            const auto event = map_ws_fill(wire, state_->instruments, received);
            if (!event.has_value()) {
                continue;
            }
            if (auto* memo = mutable_memo(wire.order_id); memo != nullptr) {
                memo->leaves = event->leaves_qty;
            }
            state_->sink->on_order_event(*event);
        }
    }

    [[nodiscard]] LiveOrder* mutable_memo(std::string_view order_id) {
        const auto it = state_->live_orders.find(std::string{order_id});
        return it == state_->live_orders.end() ? nullptr : &it->second;
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    Protocol protocol_;
    // a uuid is 36 characters; slack for a longer challenge
    std::array<char, 64> challenge_{};
    std::array<char, k_authent_bytes> signed_challenge_{};
    std::size_t challenge_len_{0};
    std::size_t signed_len_{0};
};

class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{stream_host()},
                                             .port = std::string{stream_port()},
                                             .target = std::string{stream_path()},
                                             .max_message_bytes = k_max_message_bytes},
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

} // namespace raikiri::venues::kraken::internal
