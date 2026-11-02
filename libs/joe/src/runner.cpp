#include <raikiri/joe/runner.hpp>

namespace raikiri::joe {

std::expected<std::uint64_t, SinkError> ContextSink::submit(const ChildOrder& order) noexcept {
    if (ctx_ == nullptr) {
        return std::unexpected{SinkError::NotRunning};
    }
    const auto* instrument = ctx_->instrument(order.instrument);
    if (instrument == nullptr || order.lots <= 0) {
        return std::unexpected{SinkError::Refused};
    }
    const auto ticks = base_of(*instrument).price_to_ticks(order.limit);
    if (!ticks.has_value()) {
        return std::unexpected{SinkError::Refused};
    }
    const auto sent = ctx_->submit_child(
        {.instrument = order.instrument,
         .side = order.side,
         .type = OrderType::Limit,
         .tif = order.tif == ChildTif::Ioc ? TimeInForce::Ioc : TimeInForce::Gtc,
         .flags = order.tif == ChildTif::PostOnly ? OrderFlags::PostOnly : OrderFlags::None,
         .price = *ticks,
         .qty = QtyLots::from_raw(order.lots),
         .repeat_key = order.repeat_key},
        order.book,
        order.origin);
    if (sent.has_value()) {
        return sent->raw;
    }
    return std::unexpected{sent.error() == strategy::RejectReason::Risk ? SinkError::Risk : SinkError::Refused};
}

void ContextSink::cancel(std::uint64_t child_id) noexcept {
    if (ctx_ != nullptr) {
        (void)ctx_->cancel(ClientOrderId{child_id});
    }
}

bool ContextSink::modify(std::uint64_t child_id, InstrumentId instrument, FixedE9 limit) noexcept {
    if (ctx_ == nullptr) {
        return false;
    }
    const auto* listed = ctx_->instrument(instrument);
    if (listed == nullptr) {
        return false;
    }
    const auto ticks = base_of(*listed).price_to_ticks(limit);
    return ticks.has_value() && ctx_->amend(ClientOrderId{child_id}, *ticks);
}

void Runner::on_start(strategy::Context& ctx) {
    sink_->attach(&ctx);
    ctx.set_timer(k_tick_timer, k_tick_ns);
}

void Runner::on_stop(strategy::Context&) {
    desk_->halt();
    sink_->attach(nullptr);
}

void Runner::on_timer(strategy::Context& ctx, std::uint32_t timer_id) {
    if (timer_id != k_tick_timer) {
        return;
    }
    desk_->tick();
    ctx.set_timer(k_tick_timer, k_tick_ns);
}

void Runner::on_order_event(strategy::Context&, const OrderEvent& event) {
    const auto id = event.client_order_id.raw;
    switch (event.type) {
    case OrderEventType::Accepted:
        desk_->on_ack(id);
        break;
    case OrderEventType::Filled:
        desk_->on_fill(id, event.instrument, event.qty, event.price, event.fee, event.leaves_qty);
        if (event.leaves_qty.raw == 0) {
            desk_->on_closed(id, ChildEnd::Filled);
        }
        break;
    case OrderEventType::Updated:
        desk_->on_modified(id);
        break;
    case OrderEventType::ModifyRejected:
        desk_->on_modify_refused(id);
        break;
    case OrderEventType::CancelRejected:
        desk_->on_cancel_refused(id);
        break;
    case OrderEventType::Canceled:
        desk_->on_closed(id, ChildEnd::Cancelled);
        break;
    case OrderEventType::Expired:
        desk_->on_closed(id, ChildEnd::Expired);
        break;
    case OrderEventType::Rejected:
        desk_->on_closed(id, ChildEnd::Rejected);
        break;
    case OrderEventType::Denied:
        desk_->on_closed(id, ChildEnd::Denied);
        break;
    case OrderEventType::ReconClosed:
        desk_->on_closed(id, ChildEnd::Lost);
        break;
    default:
        break;
    }
}

void Runner::on_trade(strategy::Context&, const TradeTick& trade) {
    desk_->on_trade(trade.instrument, trade.price, trade.qty);
}

} // namespace raikiri::joe
