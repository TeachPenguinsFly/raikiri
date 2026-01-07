#pragma once

// The private socket, and only that.
//
// It authenticates, subscribes and turns each private table row into an event
// on the sink. It sends no orders: an order goes out over REST and is ANSWERED
// here, and joining those two halves is the execution client's job, not this
// one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitmex/envelope.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/map.hpp>
#include <raikiri/venues/bitmex/orders.hpp>
#include <raikiri/venues/bitmex/rest.hpp>
#include <raikiri/venues/bitmex/ws.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::bitmex::detail {

// The private socket is its own connection, so it reports its own health.
constexpr std::uint32_t k_private_stream_id = 1;

OrderEventType event_type_of(OrderEffect effect) noexcept {
    switch (effect) {
    case OrderEffect::Canceled:
        return OrderEventType::Canceled;
    case OrderEffect::Rejected:
        return OrderEventType::Rejected;
    case OrderEffect::Expired:
        return OrderEventType::Expired;
    case OrderEffect::Accepted:
    case OrderEffect::Silent:
        break;
    }
    return OrderEventType::Accepted;
}

bool is_terminal(OrderEffect effect) noexcept {
    return effect == OrderEffect::Canceled || effect == OrderEffect::Rejected ||
           effect == OrderEffect::Expired;
}

// Merges one `order` row onto what this process remembered about that order and
// publishes the lifecycle event if — and only if — the status actually moved.
//
// Both the REST responses and the private socket come through here, which is
// what makes the two agree: whichever arrives first publishes, and the second
// finds `last_effect` already set and stays quiet. Without that a create whose
// response and whose stream update both said "New" would emit Accepted twice,
// and the OMS transition table treats the second as a protocol violation.
void publish_order(VenueState& state,
                   const WireOrder& wire,
                   FrameAction action,
                   UnixNanos received) {
    if (wire.order_id.empty()) {
        return;
    }
    const std::string key{wire.order_id};
    auto* memo = state.recall(key);
    const auto* instrument =
        wire.symbol.empty() ? (memo != nullptr ? state.instruments.find(memo->instrument) : nullptr)
                            : state.instruments.find(wire.symbol);
    if (instrument == nullptr) {
        // An `update` names an order id and an ordStatus and nothing else, so
        // an order this process never saw created has no instrument to
        // attribute it to. An event on the wrong instrument is worse than one
        // that never fires; reconciliation is what closes the gap.
        return;
    }
    const auto& base = base_of(*instrument);
    const auto columns = map_order_columns(wire, base, received);
    if (!columns.has_value()) {
        return; // a column that IS present failed to convert: a schema event
    }

    LiveOrder merged = memo != nullptr ? *memo : LiveOrder{};
    merged.instrument = base.id;
    if (columns->side.has_value()) {
        merged.side = *columns->side;
    }
    if (columns->order_type.has_value()) {
        merged.order_type = *columns->order_type;
    }
    if (columns->flags.has_value()) {
        merged.flags = *columns->flags;
    }
    if (columns->price.has_value()) {
        merged.price = *columns->price;
    }
    if (columns->qty.has_value()) {
        merged.qty = *columns->qty;
    }
    if (columns->leaves.has_value()) {
        merged.leaves = *columns->leaves;
    }
    if (!wire.cl_ord_id.empty()) {
        if (const auto id = ExchangeClientOrderId::from(wire.cl_ord_id); id.has_value()) {
            merged.client_order_id = *id;
        }
    }

    const auto effect = wire.ord_status.empty() ? OrderEffect::Silent : effect_of(wire.ord_status);
    // A `partial` is the working set at connect time, not news: the OMS already
    // knows these orders, and replaying them as fresh acknowledgements is what
    // reconciliation exists to avoid.
    const bool publish = action != FrameAction::Partial && effect != OrderEffect::Silent &&
                         effect != merged.last_effect;
    if (effect != OrderEffect::Silent) {
        merged.last_effect = effect;
    }
    state.remember(key, merged);

    if (publish) {
        OrderEvent event{};
        event.type = event_type_of(effect);
        event.instrument = merged.instrument;
        event.side = merged.side;
        event.order_type = merged.order_type;
        event.flags = merged.flags;
        event.exchange_client_order_id = merged.client_order_id;
        if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.price = merged.price;
        event.qty = merged.qty;
        event.leaves_qty = is_terminal(effect) ? QtyLots::from_raw(0) : merged.leaves;
        event.ts_event = columns->ts_event;
        event.ts_init = received;
        state.sink->on_order_event(event);
    }
    // Nothing more will be said about a finished order, and the memo is bounded.
    if (is_terminal(effect) || wire.ord_status == "Filled") {
        state.live_orders.erase(key);
    }
}

void publish_executions(VenueState& state, std::string_view payload, UnixNanos received) {
    static thread_local const auto storage_owner = std::make_unique<ExecutionBuffer>();
    auto& storage = *storage_owner;
    const auto rows = parse_executions(payload, storage);
    if (!rows.has_value()) {
        return;
    }
    for (const auto& wire : *rows) {
        // Funding and Settlement rows carry a price and a quantity too; only
        // execType "Trade" is a fill, and map_execution is where that is read.
        const auto event = map_execution(wire, state.instruments, received);
        if (!event.has_value()) {
            continue;
        }
        if (auto* memo = state.recall(std::string{wire.order_id}); memo != nullptr) {
            memo->leaves = event->leaves_qty;
        }
        state.sink->on_order_event(*event);
    }
}

// BitMEX's private tables ride the same endpoint as the public ones but behind
// a signed handshake, so this is a second connection: a market-data reconnect
// must not have to re-authenticate, and an order stream must not drop because a
// book resubscribe failed.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    void on_open() override {
        const auto expires =
            state_->clock->wall().ns / 1'000'000'000ULL + SignedRequestBuilder::k_expiry_window_s;
        std::array<char, 64> message{};
        const auto text = auth_message(expires, message);
        std::array<char, net::k_hex_bytes> digest{};
        const auto signature = text.empty()
                                   ? std::string_view{}
                                   : state_->signer.sign(text, net::Signer::Encoding::Hex, digest);
        const auto frame = protocol_.auth_frame(state_->signer.api_key(), expires, signature);
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        // Sent without waiting for an ack on purpose: BitMEX processes a
        // connection's messages in order, and the refusal path is an error
        // frame rather than a silent drop of the subscribe.
        const std::array<std::string_view, 3> tables{
            k_table_order, k_table_execution, k_table_position};
        const auto subscribe = protocol_.private_subscribe_frame(tables);
        if (!subscribe.has_value() || !client_->send(subscribe->text)) {
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
        const auto info = parse_frame(payload);
        if (!info.has_value()) {
            return; // "pong" is not JSON, and neither is a frame BitMEX adds
        }
        switch (info->table) {
        case TableKind::Order: {
            static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
            auto& storage = *storage_owner;
            const auto rows = parse_orders(payload, storage);
            if (!rows.has_value()) {
                return;
            }
            for (const auto& wire : *rows) {
                publish_order(*state_, wire, info->action, received);
            }
            return;
        }
        case TableKind::Execution:
            publish_executions(*state_, payload, received);
            return;
        case TableKind::Position:
            // Positions are pulled, not streamed onward: GatewaySink carries no
            // position channel, and a reconciler asks for the whole picture.
            return;
        case TableKind::Error: {
            const auto err = parse_stream_error(payload);
            const auto status = err.has_value() ? err->status : k_unauthorized;
            state_->last_error_status = status;
            // A refused handshake looks exactly like an idle account otherwise:
            // the socket stays open and simply never carries a private row.
            state_->sink->on_connectivity(
                {.state = classify(status, {}) == exec::ErrorClass::AuthFailure
                              ? exec::ConnState::AuthFailed
                              : exec::ConnState::Disconnected,
                 .stream_id = k_private_stream_id,
                 .ts = state_->clock->wall()});
            return;
        }
        case TableKind::SubscribeAck:
        case TableKind::Info:
        case TableKind::Quote:
        case TableKind::Trade:
        case TableKind::Instrument:
        case TableKind::OrderBook:
        case TableKind::Unknown:
            // Public tables are never subscribed on this socket. Naming them is
            // what makes a new table a compiler error rather than a dropped one.
            return;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
    Protocol protocol_;
};

// The socket itself, owned as one thing so a caller cannot hold the handler
// without the client it is attached to.
class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, client_{state.io,
                                   net::WsConfig{.host = std::string{stream_host(state.testnet)},
                                                 .port = std::string{stream_port()},
                                                 .target = std::string{stream_path()}},
                                   handler_} {
        handler_.attach(client_);
    }

    void start() { client_.start(); }

    void stop() { client_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return client_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient client_;
};

} // namespace raikiri::venues::bitmex::detail
