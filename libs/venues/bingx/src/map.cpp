#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/map.hpp>

#include <optional>

namespace raikiri::venues::bingx {

namespace {

// FixedE9 holds nine fractional digits, so a precision past nine has no exact
// step and the row is refused rather than approximated.
constexpr std::int64_t k_max_precision = 9;

constexpr std::uint32_t k_seconds_per_hour = 3600;

// /openApi/spot/v1/common/symbols answers 1 for a listed pair; 0, 10 and 25
// are the states it is not tradable in.
constexpr std::int64_t k_spot_online = 1;

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means BingX changed a contract's precision, so the message is
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

// BingX names a spot pair and a perpetual IDENTICALLY, so every mapper states
// the market it reads and a frame that resolves to the other one is dropped
// rather than mapped. The market-data mappers take it from the caller — the
// socket knows which tree it connected to. Funding and positions pin the swap:
// a spot pair has neither.
const InstrumentBase*
base_for_market(std::string_view symbol, const exec::InstrumentTable& table, MarketType market) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return nullptr;
    }
    const auto& base = base_of(*instrument);
    return base.id.market() == market ? &base : nullptr;
}

// Both trees state the ORIGINAL and the CUMULATIVE filled size and never the
// remainder. Floored at zero: a venue that overstates the fill must not
// produce a negative quantity nobody downstream checks for.
QtyLots leaves_from(std::string_view orig, std::string_view filled, const InstrumentBase& base) {
    const auto total = to_lots(orig, base);
    if (!total.has_value()) {
        return QtyLots::from_raw(0);
    }
    const auto done = to_lots(filled, base);
    if (!done.has_value()) {
        return *total;
    }
    return QtyLots::from_raw(total->raw > done->raw ? total->raw - done->raw : 0);
}

bool copy_id(std::string_view text, ExchangeOrderId& out) {
    const auto id = ExchangeOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// Exponent-aware: the spot catalogue writes its steps as "1e-06", which
// parse_decimal_e9 cannot read.
std::optional<FixedE9> to_number_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    return parse_number_e9(text);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept {
    if (precision < 0 || precision > k_max_precision) {
        return std::nullopt;
    }
    std::int64_t divisor = 1;
    for (std::int64_t i = 0; i < precision; ++i) {
        divisor *= 10;
    }
    return FixedE9::from_raw(k_e9 / divisor);
}

std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, MarketType market,
         UnixNanos ts_init) {
    const auto* base_ptr = base_for_market(wire.symbol, table, market);
    if (base_ptr == nullptr) {
        return std::nullopt;
    }
    const auto& base = *base_ptr;
    const auto bid = to_ticks(wire.bid_price, base);
    const auto ask = to_ticks(wire.ask_price, base);
    const auto bid_qty = to_lots(wire.bid_qty, base);
    const auto ask_qty = to_lots(wire.ask_qty, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.transaction_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, MarketType market,
         UnixNanos ts_init) {
    const auto* base_ptr = base_for_market(wire.symbol, table, market);
    if (base_ptr == nullptr) {
        return std::nullopt;
    }
    const auto& base = *base_ptr;
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    // The flip: `m` says the BUYER was the maker, so the aggressor sold.
    out.aggressor = wire.buyer_is_maker ? Side::Sell : Side::Buy;
    out.ts_event = ms_to_unix(wire.trade_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, MarketType market,
         UnixNanos ts_init) {
    const auto* base_ptr = base_for_market(wire.symbol, table, market);
    if (base_ptr == nullptr) {
        return std::nullopt;
    }
    const auto& base = *base_ptr;

    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;

    const auto fill = [&](std::span<const WireLevel> src,
                          std::array<BookLevel, k_book_levels>& dst) {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, base);
            const auto qty = to_lots(level.qty, base);
            // A level that does not divide by the instrument's step truncates
            // the book here rather than being rounded into a price BingX never
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

std::optional<FundingUpdate>
map_funding(const WirePremiumIndex& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base_ptr = base_for_market(wire.symbol, table, MarketType::Perp);
    if (base_ptr == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.last_funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = *base_ptr;

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    if (wire.funding_interval_hours > 0) {
        out.interval_s =
            static_cast<std::uint32_t>(wire.funding_interval_hours) * k_seconds_per_hour;
    }
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    if (wire.status != 1) {
        return std::nullopt;
    }
    if (market != MarketType::Perp) {
        return std::nullopt;
    }

    const auto tick = step_from_precision(wire.price_precision);
    const auto lot = step_from_precision(wire.quantity_precision);
    if (!tick.has_value() || !lot.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Bingx, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    if (const auto min_notional = to_e9(wire.trade_min_usdt); min_notional.has_value()) {
        base.min_notional = *min_notional;
    }
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire) {
    // Two gates, not one: a pair can be listed and priced for the web and
    // still be closed to API orders, and an order on one is refused.
    if (wire.status != k_spot_online || !wire.api_buy || !wire.api_sell) {
        return std::nullopt;
    }
    const auto tick = to_number_e9(wire.tick_size);
    const auto lot = to_number_e9(wire.step_size);
    // Nine live rows quote a tick of 1e-12. FixedE9 has no such value, and a
    // step rounded up to 1e-9 is a price grid BingX does not use.
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Bingx, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto notional = to_number_e9(wire.min_notional); notional.has_value()) {
        base.min_notional = *notional;
    }
    base.contract_value = *parse_decimal_e9("1"); // spot quotes base units
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table, MarketType market) {
    const auto* base = base_for_market(wire.symbol, table, market);
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
    if (const auto coid = client_id_from_text(wire.client_order_id); coid.has_value()) {
        out.exchange_client_order_id = *coid;
    }
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
        // PENDING counts here: BingX's own examples show a resting limit order
        // in that state, and reading it as anything else adopts a working
        // order as finished.
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderStatus::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    case WireOrderStatus::Filled:
        out.status = OrderStatus::Filled;
        break;
    case WireOrderStatus::Canceled:
        out.status = OrderStatus::Canceled;
        break;
    case WireOrderStatus::Expired:
        out.status = OrderStatus::Expired;
        break;
    case WireOrderStatus::Rejected:
        out.status = OrderStatus::Rejected;
        break;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<OrderEvent> map_order_update(const WireOrderUpdate& wire,
                                           const exec::InstrumentTable& table,
                                           MarketType market,
                                           const ExchangeClientOrderId& fallback_client_id,
                                           UnixNanos ts_init) {
    const auto* base = base_for_market(wire.symbol, table, market);
    if (base == nullptr) {
        return std::nullopt;
    }

    // `x` is what happened, `X` where the order stands after. BingX has been
    // seen answering "Unknown" for `X` on a frame whose `x` said TRADE.
    OrderEventType type{};
    if (wire.exec_type == "NEW") {
        type = OrderEventType::Accepted;
    } else if (wire.exec_type == "TRADE") {
        type = OrderEventType::Filled;
    } else if (wire.exec_type == "CANCELED") {
        type = OrderEventType::Canceled;
    } else if (wire.exec_type == "EXPIRED") {
        type = OrderEventType::Expired;
    } else {
        // CALCULATED is BingX's ADL/liquidation bookkeeping, not an execution,
        // and an unknown type is not something to guess a lifecycle from.
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base->id;
    out.side = wire.side == "SELL" ? Side::Sell : Side::Buy;
    out.order_type = wire.type == "MARKET" ? OrderType::Market : OrderType::Limit;
    if (const auto coid = client_id_from_text(wire.client_order_id); coid.has_value()) {
        out.exchange_client_order_id = *coid;
    } else {
        out.exchange_client_order_id = fallback_client_id;
    }
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.leaves_qty = leaves_from(wire.orig_qty, wire.cum_qty, *base);
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;

    if (type != OrderEventType::Filled) {
        if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
            out.price = *price;
        }
        if (const auto qty = to_lots(wire.orig_qty, *base); qty.has_value()) {
            out.qty = *qty;
        }
        if (type == OrderEventType::Accepted) {
            out.leaves_qty = out.qty;
        }
        return out;
    }

    // A fill states its own size and price. BingX's coin-M copy of this push
    // omits both, and reporting the cumulative at the AVERAGE price would
    // invent a trade at a price nothing matched at.
    const auto price = to_ticks(wire.last_price, *base);
    const auto qty = to_lots(wire.last_qty, *base);
    if (!price.has_value() || !qty.has_value() || qty->raw <= 0) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    if (const auto fee = to_e9(wire.fee); fee.has_value()) {
        out.fee = *fee;
    }
    if (wire.maker_known) {
        out.liquidity = wire.is_maker ? Liquidity::Maker : Liquidity::Taker;
    }
    // Spot states a trade id and the perpetual push mints none, so this stays
    // empty rather than holding an id BingX never issued.
    if (!wire.trade_id.empty() && wire.trade_id != "0") {
        if (const auto id = ExecId::from(wire.trade_id); id.has_value()) {
            out.exec_id = *id;
        }
    }
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* base = base_for_market(wire.symbol, table, MarketType::Perp);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto size = to_lots(wire.position_amt, *base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base->id;
    // positionAmt is unsigned on both sides of the book, so the side is the
    // only thing that says which way the position points.
    out.net_qty = QtyLots::from_raw(wire.position_side == "SHORT" ? -size->raw : size->raw);
    if (const auto entry = to_ticks(wire.avg_price, *base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

} // namespace raikiri::venues::bingx
