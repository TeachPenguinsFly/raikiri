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
    const auto sent = ctx_->submit({.instrument = order.instrument,
                                    .side = order.side,
                                    .type = OrderType::Limit,
                                    .tif = TimeInForce::Ioc,
                                    .flags = OrderFlags::None,
                                    .price = *ticks,
                                    .qty = QtyLots::from_raw(order.lots)});
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
    case OrderEventType::Filled:
        desk_->on_fill(id, event.instrument, event.qty, event.price, event.fee);
        if (event.leaves_qty.raw == 0) {
            desk_->on_closed(id);
        }
        break;
    case OrderEventType::Canceled:
    case OrderEventType::Expired:
    case OrderEventType::Rejected:
    case OrderEventType::Denied:
    case OrderEventType::ReconClosed:
        desk_->on_closed(id);
        break;
    default:
        break;
    }
}

void Runner::on_trade(strategy::Context&, const TradeTick& trade) {
    desk_->on_trade(trade.instrument, trade.price, trade.qty);
}

} // namespace raikiri::joe
