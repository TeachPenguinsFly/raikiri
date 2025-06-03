#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/mexc/map.hpp>

#include <optional>

namespace raikiri::venues::mexc {

namespace {

// /contract/detail's `state`: 0 is the enabled value and the only one seen
// across all 1086 rows on 2026-08-10.
constexpr std::int64_t k_state_enabled = 0;

// FixedE9 carries nine fractional digits.
constexpr std::int64_t k_max_scale = 9;

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means MEXC changed an instrument's precision, so the message is
// dropped rather than rounded — rounding is how an order ends up one tick away
// from the intended price.
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

// Rates are venue-scale decimals, not instrument-scaled, so they keep their
// 1e9 representation rather than becoming ticks.
std::optional<FixedE9> to_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? std::optional{*value} : std::nullopt;
}

const InstrumentBase* base_for(std::string_view symbol, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_from_scale(std::int64_t scale) noexcept {
    if (scale < 0 || scale > k_max_scale) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::int64_t i = 0; i < scale; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Bbo>
map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!wire.snapshot || wire.bids.empty() || wire.asks.empty()) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto bid = to_ticks(wire.bids.front().price, *base);
    const auto ask = to_ticks(wire.asks.front().price, *base);
    const auto bid_qty = to_lots(wire.bids.front().qty, *base);
    const auto ask_qty = to_lots(wire.asks.front().qty, *base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base->id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!wire.snapshot) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    BookSnapshot out{};
    out.instrument = base->id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    if (wire.version > 0) {
        out.venue_seq = static_cast<std::uint64_t>(wire.version);
    }

    const auto fill = [&](std::span<const WireLevel> src,
                          std::array<BookLevel, k_book_levels>& dst) {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, *base);
            const auto qty = to_lots(level.qty, *base);
            // A level that does not divide by the instrument's step truncates
            // the book here rather than being rounded into a price MEXC never
            // quoted.
            if (!price.has_value() || !qty.has_value()) {
                break;
            }
            dst[n] = {.price = *price, .qty = *qty};
            ++n;
        }
        return n;
    };
    out.bid_count = fill(wire.bids, out.bids);
    out.ask_count = fill(wire.asks, out.asks);
    if (out.bid_count == 0 && out.ask_count == 0) {
        return std::nullopt;
    }
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.qty, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base->id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base->id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.fair_price, *base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }

    FundingUpdate out{};
    out.instrument = base->id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.fair_price, *base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_settle_ms, UnixNanos{0});
    if (wire.collect_cycle_h > 0) {
        out.interval_s = static_cast<std::uint32_t>(wire.collect_cycle_h * 3600);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.state != k_state_enabled) {
        return std::nullopt;
    }

    const auto tick = step_from_scale(wire.price_scale);
    const auto lot = step_from_scale(wire.vol_scale);
    if (!tick.has_value() || !lot.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }
    const auto contract_size = parse_decimal_e9(wire.contract_size);
    if (!contract_size.has_value() || contract_size->raw <= 0) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Mexc, MarketType::Perp, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *contract_size;
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire) {
    // `status` is the STRING "1" on a trading row; the tradability gate is
    // isSpotTradingAllowed, which is false on rows MEXC still prices.
    if (wire.status != "1" || !wire.spot_trading_allowed) {
        return std::nullopt;
    }
    const auto tick = step_from_scale(wire.quote_precision);
    if (!tick.has_value()) {
        return std::nullopt;
    }
    // The quantity step arrives as a decimal, not a digit count: the two
    // precisions in one row are stated in different forms.
    const auto lot = parse_decimal_e9(wire.base_size_step);
    if (!lot.has_value() || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Mexc, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    // A spot lot IS one base unit; there is no contract multiplier to carry.
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

// ------------------------------------------------------------- private ----

namespace {

// MEXC futures says 1 holding, 2 system auto-holding, 3 closed.
constexpr std::int64_t k_position_closed = 3;

// MEXC futures order states. 2 covers New and PartiallyFilled alike.
constexpr std::int64_t k_order_pending = 1;
constexpr std::int64_t k_order_working = 2;
constexpr std::int64_t k_order_filled = 3;
constexpr std::int64_t k_order_cancelled = 4;
constexpr std::int64_t k_order_invalid = 5;

// Our own ULID coming back from the venue. Anything that is not one — MEXC's
// own web orders carry a 35-character `_m_<hex>` externalOid — leaves the field
// UNSET rather than dropping the event: an order we did not place is still an
// order the reconciler has to see, and an id that does not parse is never
// truncated into a lookalike of one that does.
void copy_client_id(std::string_view text, ExchangeClientOrderId& out) {
    if (const auto id = ExchangeClientOrderId::from(text); id.has_value()) {
        out = *id;
    }
}

bool copy_id(std::string_view text, ExchangeOrderId& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = ExchangeOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

bool copy_exec_id(std::string_view text, ExecId& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = ExecId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// Ordered minus dealt, floored at zero. A venue that reports more dealt than
// ordered is a schema event; reading the difference as unsigned would turn it
// into an enormous resting size.
QtyLots leaves_from(std::string_view ordered, std::string_view dealt, const InstrumentBase& base) {
    const auto total = to_lots(ordered, base);
    if (!total.has_value()) {
        return QtyLots::from_raw(0);
    }
    const auto done = to_lots(dealt, base);
    const auto left = total->raw - (done.has_value() ? done->raw : 0);
    return QtyLots::from_raw(left > 0 ? left : 0);
}

} // namespace

Side side_from_futures(std::int64_t side) noexcept {
    // 1 open long, 2 close short — both buy. 3 open short, 4 close long.
    return (side == 1 || side == 2) ? Side::Buy : Side::Sell;
}

std::optional<OrderEvent> map_futures_order_event(const WireFuturesOrder& wire,
                                                  const exec::InstrumentTable& table,
                                                  UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    OrderEventType type{};
    if (wire.state == k_order_pending) {
        return std::nullopt; // "uninformed": MEXC has not decided yet
    } else if (wire.state == k_order_working) {
        // State 2 covers both New and PartiallyFilled and only dealVol tells
        // them apart. A partial's lifecycle belongs to its deal frame.
        const auto dealt = to_lots(wire.deal_vol, *base);
        if (dealt.has_value() && dealt->raw > 0) {
            return std::nullopt;
        }
        type = OrderEventType::Accepted;
    } else if (wire.state == k_order_filled) {
        return std::nullopt; // the deal frame carries the money
    } else if (wire.state == k_order_cancelled) {
        type = OrderEventType::Canceled;
    } else if (wire.state == k_order_invalid) {
        type = OrderEventType::Rejected;
    } else {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base->id;
    out.side = side_from_futures(wire.side);
    out.order_type = wire.order_type == 5 ? OrderType::Market : OrderType::Limit;
    copy_client_id(wire.external_oid, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.vol, *base); qty.has_value()) {
        out.qty = *qty;
    }
    out.leaves_qty =
        type == OrderEventType::Accepted ? out.qty : leaves_from(wire.vol, wire.deal_vol, *base);
    out.ts_event = ms_to_unix(wire.update_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_futures_deal(const WireFuturesDeal& wire,
                                           const exec::InstrumentTable& table,
                                           QtyLots leaves,
                                           const ExchangeClientOrderId& client_id,
                                           UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.vol, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base->id;
    out.side = side_from_futures(wire.side);
    out.exchange_client_order_id = client_id;
    if (!copy_id(wire.order_id, out.exchange_order_id) ||
        !copy_exec_id(wire.deal_id, out.exec_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = leaves;
    if (const auto fee = to_e9(wire.fee); fee.has_value()) {
        out.fee = *fee;
    }
    // A fill whose liquidity MEXC did not state stays Unknown: guessing Taker
    // is how a maker strategy scores itself against a number it invented.
    out.liquidity = wire.liquidity_known ? (wire.taker ? Liquidity::Taker : Liquidity::Maker)
                                         : Liquidity::Unknown;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_futures_open_order(const WireFuturesOrder& wire,
                                                              const exec::InstrumentTable& table) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto qty = to_lots(wire.vol, *base);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    const auto leaves = leaves_from(wire.vol, wire.deal_vol, *base);

    exec::OrderStatusReport out{};
    out.instrument = base->id;
    out.side = side_from_futures(wire.side);
    copy_client_id(wire.external_oid, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves = leaves;
    out.status = wire.state == k_order_cancelled ? OrderStatus::Canceled
                 : wire.state == k_order_filled  ? OrderStatus::Filled
                 : wire.state == k_order_invalid ? OrderStatus::Rejected
                 : leaves.raw < qty->raw         ? OrderStatus::PartiallyFilled
                                                 : OrderStatus::Accepted;
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<exec::PositionReport> map_futures_position(const WireFuturesPosition& wire,
                                                         const exec::InstrumentTable& table) {
    if (wire.state == k_position_closed) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto size = to_lots(wire.hold_vol, *base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base->id;
    // holdVol is UNSIGNED; positionType is the only thing that says which way.
    out.net_qty = QtyLots::from_raw(wire.position_type == 2 ? -size->raw : size->raw);
    if (const auto entry = to_ticks(wire.hold_avg_price, *base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<OrderEvent> map_spot_order_event(const WireSpotOrder& wire,
                                               const exec::InstrumentTable& table,
                                               UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    OrderEventType type{};
    switch (wire.status) {
    case WireOrderStatus::New:
        type = OrderEventType::Accepted;
        break;
    case WireOrderStatus::Cancelled:
        type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::Rejected:
        type = OrderEventType::Rejected;
        break;
    case WireOrderStatus::Expired:
        type = OrderEventType::Expired;
        break;
    case WireOrderStatus::PartiallyFilled:
    case WireOrderStatus::Filled:
        // The fill is the trade record's to report; this frame only restates it.
        return std::nullopt;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base->id;
    out.side = wire.side == "SELL" ? Side::Sell : Side::Buy;
    out.order_type = wire.type == "MARKET" ? OrderType::Market : OrderType::Limit;
    if (wire.type == "LIMIT_MAKER") {
        out.flags = OrderFlags::PostOnly;
    }
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.orig_qty, *base); qty.has_value()) {
        out.qty = *qty;
    }
    out.leaves_qty = type == OrderEventType::Accepted
                         ? out.qty
                         : leaves_from(wire.orig_qty, wire.executed_qty, *base);
    out.ts_event = ms_to_unix(wire.update_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_spot_open_order(const WireSpotOrder& wire,
                                                           const exec::InstrumentTable& table) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto qty = to_lots(wire.orig_qty, *base);
    if (!qty.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base->id;
    out.side = wire.side == "SELL" ? Side::Sell : Side::Buy;
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves = leaves_from(wire.orig_qty, wire.executed_qty, *base);
    switch (wire.status) {
    case WireOrderStatus::New:
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderStatus::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    case WireOrderStatus::Filled:
        out.status = OrderStatus::Filled;
        break;
    case WireOrderStatus::Cancelled:
        out.status = OrderStatus::Canceled;
        break;
    case WireOrderStatus::Rejected:
        out.status = OrderStatus::Rejected;
        break;
    case WireOrderStatus::Expired:
        out.status = OrderStatus::Expired;
        break;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<OrderEvent> map_spot_trade(const WireSpotTrade& wire,
                                         const exec::InstrumentTable& table,
                                         QtyLots leaves,
                                         UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.qty, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base->id;
    out.side = wire.is_buyer ? Side::Buy : Side::Sell;
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id) ||
        !copy_exec_id(wire.trade_id, out.exec_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = leaves;
    if (const auto fee = to_e9(wire.commission); fee.has_value()) {
        out.fee = *fee;
    }
    out.liquidity = wire.is_maker ? Liquidity::Maker : Liquidity::Taker;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::mexc
