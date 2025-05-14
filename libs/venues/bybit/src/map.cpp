#include <raikiri/exec/execution_client.hpp>
#include <raikiri/venues/bybit/map.hpp>

#include <charconv>

namespace raikiri::venues::bybit {

namespace {

// Exact conversion against the instrument's own step. A venue value that
// does not divide evenly means bybit changed a filter, so the message is
// dropped rather than rounded — rounding here is how an order ends up at a
// price the venue will reject, or worse, one tick away from the intended one.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

std::optional<PriceTicks> to_ticks(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

// Fees and funding rates are venue-scale decimals, not instrument-scaled, so
// they keep their 1e9 representation rather than becoming ticks.
FixedE9 to_e9(std::string_view text) {
    if (text.empty()) {
        return FixedE9::from_raw(0);
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? *value : FixedE9::from_raw(0);
}

// Epoch milliseconds as digits. Zero means "no usable time", which callers
// resolve to ingest time rather than to 1970.
std::uint64_t digits_to_ms(std::string_view text) noexcept {
    if (text.empty()) {
        return 0;
    }
    std::uint64_t out = 0;
    const auto* const end = text.data() + text.size();
    const auto [stop, ec] = std::from_chars(text.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
}

bool copy_id(std::string_view text, ExchangeOrderId& out) {
    if (text.empty()) {
        return true; // absent is fine; wrong is not
    }
    const auto id = ExchangeOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// Our own ULID coming back from the venue. A value that does not fit the
// field cannot be one of ours, so the event stays unattributed rather than
// being matched to a truncated lookalike.
bool copy_client_id(std::string_view text, ExchangeClientOrderId& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = ExchangeClientOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

Side side_from(std::string_view text) noexcept {
    return text == "Sell" ? Side::Sell : Side::Buy;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt; // a symbol we never subscribed to
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.size, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = wire.buyer_aggressor ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.funding_rate.empty() && wire.next_funding_ms.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = to_e9(wire.funding_rate);
    out.predicted_rate = FixedE9::from_raw(0); // bybit publishes no forecast here
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    if (const auto ms = digits_to_ms(wire.next_funding_ms); ms != 0) {
        out.next_funding_time = UnixNanos{ms * 1'000'000ULL};
    }
    if (const auto* perp = std::get_if<PerpInstrument>(instrument); perp != nullptr) {
        out.interval_s = perp->funding_interval_s;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_order_event(const WireOrderUpdate& wire,
                                          const exec::InstrumentTable& table,
                                          UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    OrderEventType type{};
    switch (wire.status) {
    case WireOrderStatus::New:
    case WireOrderStatus::Untriggered:
        type = OrderEventType::Accepted;
        break;
    case WireOrderStatus::Triggered:
        type = OrderEventType::Triggered;
        break;
    case WireOrderStatus::Rejected:
        type = OrderEventType::Rejected;
        break;
    case WireOrderStatus::Cancelled:
    case WireOrderStatus::Deactivated:
        type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::PartiallyFilledCanceled:
        type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::PartiallyFilled:
    case WireOrderStatus::Filled:
        return std::nullopt;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.order_type = wire.order_type == "Market" ? OrderType::Market : OrderType::Limit;
    if (!copy_client_id(wire.order_link_id, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.qty, base); qty.has_value()) {
        out.qty = *qty;
    }
    if (const auto leaves = to_lots(wire.leaves_qty, base); leaves.has_value()) {
        out.leaves_qty = *leaves;
    }
    out.ts_event = ms_to_unix(wire.updated_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent>
map_execution(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.exec_price, base);
    const auto qty = to_lots(wire.exec_qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.liquidity = wire.is_maker ? 1 : 2;
    if (!copy_client_id(wire.order_link_id, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (!wire.exec_id.empty()) {
        const auto exec_id = ExecId::from(wire.exec_id);
        if (!exec_id.has_value()) {
            return std::nullopt; // dedupe key we cannot store is not usable
        }
        out.exec_id = *exec_id;
    }
    out.price = *price;
    out.qty = *qty;
    if (const auto leaves = to_lots(wire.leaves_qty, base); leaves.has_value()) {
        out.leaves_qty = *leaves;
    } else {
        out.leaves_qty = QtyLots::from_raw(0);
    }
    out.fee = to_e9(wire.exec_fee);
    out.ts_event = ms_to_unix(wire.exec_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOpenOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    if (!copy_client_id(wire.order_link_id, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    switch (wire.status) {
    case WireOrderStatus::New:
    case WireOrderStatus::Untriggered:
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderStatus::Triggered:
        out.status = OrderStatus::Triggered;
        break;
    case WireOrderStatus::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    default:
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.qty, base); qty.has_value()) {
        out.qty = *qty;
    }
    if (const auto leaves = to_lots(wire.leaves_qty, base); leaves.has_value()) {
        out.leaves = *leaves;
    }
    out.ts_venue = ms_to_unix(wire.updated_ms, UnixNanos{});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto size = to_lots(wire.size, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = wire.side == "Sell" ? QtyLots::from_raw(-size->raw) : *size;
    if (const auto avg = to_ticks(wire.avg_price, base); avg.has_value()) {
        out.avg_entry = *avg;
    }
    out.ts_venue = ms_to_unix(wire.updated_ms, UnixNanos{});
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (wire.status != "Trading") {
        return std::nullopt; // delisted or pre-launch: not tradable today
    }
    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.qty_step);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        // Every later price conversion divides by these.
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bybit, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = to_e9(wire.min_notional);
    base.contract_value = *parse_decimal_e9("1"); // linear perps quote in base units
    base.kind = ContractKind::Linear;
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }
    return PerpInstrument{base, /*funding_interval_s=*/28'800};
}

} // namespace raikiri::venues::bybit
