#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/lighter/lifecycle.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>

namespace raikiri::venues::lighter {

namespace {

constexpr std::size_t k_seen_trades_cap = 100'000;

[[nodiscard]] std::optional<std::int64_t> steps(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

template <typename Id> [[nodiscard]] Id decimal_id(std::uint64_t value) noexcept {
    std::array<char, 24> text{};
    const auto [stop, ec] = std::to_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value == 0) {
        return {};
    }
    const auto id = Id::from(std::string_view{text.data(), static_cast<std::size_t>(stop - text.data())});
    return id.has_value() ? *id : Id{};
}

} // namespace

TradeSide side_of(const WireTrade& trade, std::int64_t account_index) noexcept {
    const bool ask = trade.ask_account_id == account_index;
    return {.order_index = ask ? trade.ask_id : trade.bid_id,
            .client_index = ask ? trade.ask_client_id : trade.bid_client_id,
            .is_ask = ask,
            .maker = ask == trade.is_maker_ask};
}

ExchangeOrderId order_id_of(std::uint64_t order_index) noexcept {
    return decimal_id<ExchangeOrderId>(order_index);
}

ExecId trade_id_of(std::uint64_t trade_id) noexcept {
    return decimal_id<ExecId>(trade_id);
}

void Lifecycle::track(const OrderCmd& cmd) {
    orders_[client_order_index(cmd.exchange_client_order_id)] = Tracked{.cmd = cmd};
}

void Lifecycle::forget(const ExchangeClientOrderId& id) noexcept {
    orders_.erase(client_order_index(id));
}

const OrderCmd* Lifecycle::find(std::int64_t client_index) const noexcept {
    const auto found = orders_.find(client_index);
    return found == orders_.end() ? nullptr : &found->second.cmd;
}

OrderEvent
Lifecycle::event_of(const Tracked& tracked, OrderEventType type, QtyLots leaves, UnixNanos received) const noexcept {
    OrderEvent event{};
    event.type = type;
    event.side = tracked.cmd.side;
    event.order_type = tracked.cmd.order_type;
    event.tif = tracked.cmd.tif;
    event.flags = tracked.cmd.flags;
    event.instrument = tracked.cmd.instrument;
    event.client_order_id = tracked.cmd.client_order_id;
    event.exchange_client_order_id = tracked.cmd.exchange_client_order_id;
    event.exchange_order_id = order_id_of(tracked.order_index);
    event.price = tracked.cmd.price;
    event.qty = tracked.cmd.qty;
    event.leaves_qty = leaves;
    event.ts_event = received;
    event.ts_init = received;
    return event;
}

void Lifecycle::accept_once(Tracked& tracked, UnixNanos received) noexcept {
    if (tracked.accepted) {
        return;
    }
    tracked.accepted = true;
    sink_->on_order_event(event_of(tracked, OrderEventType::Accepted, tracked.cmd.qty, received));
}

bool Lifecycle::try_close(Tracked& tracked, UnixNanos received) noexcept {
    if (tracked.closed_at_filled < 0 || tracked.filled < tracked.closed_at_filled) {
        return false;
    }
    sink_->on_order_event(event_of(tracked, tracked.close, QtyLots::from_raw(0), received));
    return true;
}

void Lifecycle::on_order(const WireOrder& row, UnixNanos received) noexcept {
    const auto found = orders_.find(row.client_order_index);
    if (found == orders_.end()) {
        return;
    }
    auto& tracked = found->second;
    tracked.order_index = row.order_index;
    switch (order_phase(row.status)) {
    case OrderPhase::Live:
        accept_once(tracked, received);
        return;
    case OrderPhase::Filled:
        accept_once(tracked, received);
        if (tracked.filled >= tracked.cmd.qty.raw) {
            orders_.erase(found);
        }
        return;
    case OrderPhase::PostOnlyCrossed: {
        auto event = event_of(tracked, OrderEventType::Rejected, QtyLots::from_raw(0), received);
        event.reject_class = ErrorClass::Terminal;
        sink_->on_order_error(tracked.cmd.client_order_id,
                              exec::VenueError::make(ErrorClass::Terminal, 0, "post-only order would have crossed"));
        sink_->on_order_event(event);
        orders_.erase(found);
        return;
    }
    case OrderPhase::Canceled: {
        accept_once(tracked, received);
        const auto* instrument = instruments_->find(tracked.cmd.instrument);
        const auto filled = instrument == nullptr ? std::nullopt
                                                  : steps(row.filled_base_amount, base_of(*instrument).lot_size);
        // an immediate order's unfilled rest is canceled by the matcher, not by us
        const bool immediate = tracked.cmd.tif == TimeInForce::Ioc || tracked.cmd.order_type == OrderType::Market;
        tracked.close = immediate ? OrderEventType::Expired : OrderEventType::Canceled;
        tracked.closed_at_filled = filled.value_or(0);
        if (try_close(tracked, received)) {
            orders_.erase(found);
        }
        return;
    }
    case OrderPhase::Unknown:
        return;
    }
}

void Lifecycle::on_trade(const WireTrade& trade, UnixNanos received) noexcept {
    if (!seen_trades_.insert(trade.trade_id).second) {
        return;
    }
    if (seen_trades_.size() > k_seen_trades_cap) {
        seen_trades_.clear();
    }
    const auto ours = side_of(trade, account_);
    const auto found = orders_.find(ours.client_index);
    if (found == orders_.end()) {
        return;
    }
    auto& tracked = found->second;
    const auto* instrument = instruments_->find(tracked.cmd.instrument);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    const auto price = steps(trade.price, base.tick_size);
    const auto qty = steps(trade.size, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return;
    }
    tracked.order_index = ours.order_index;
    accept_once(tracked, received);
    tracked.filled += *qty;
    const auto leaves = QtyLots::from_raw(std::max<std::int64_t>(tracked.cmd.qty.raw - tracked.filled, 0));
    auto fill = event_of(tracked, OrderEventType::Filled, leaves, received);
    fill.price = PriceTicks::from_raw(*price);
    fill.qty = QtyLots::from_raw(*qty);
    fill.exec_id = trade_id_of(trade.trade_id);
    fill.liquidity = ours.maker ? Liquidity::Maker : Liquidity::Taker;
    fill.fee = fee_of(trade, ours.maker);
    fill.ts_event = UnixNanos{trade.ts_ms * 1'000'000ULL};
    sink_->on_order_event(fill);
    if (leaves.raw == 0 || try_close(tracked, received)) {
        orders_.erase(found);
    }
}

} // namespace raikiri::venues::lighter
