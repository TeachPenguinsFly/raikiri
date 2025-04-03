#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/binance/map.hpp>

#include <optional>

namespace raikiri::venues::binance {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means binance changed a filter, so the message is dropped
// rather than rounded — rounding is how an order ends up one tick away from
// the intended price.
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
FixedE9 to_e9(std::string_view text) {
    if (text.empty()) {
        return FixedE9::from_raw(0);
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? *value : FixedE9::from_raw(0);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
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
    // T, not E: the transaction time is when the book actually changed, while
    // E is when binance got around to sending it. Using E would fold venue
    // queueing delay into our own latency numbers.
    out.ts_event = ms_to_unix(wire.transaction_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireAggTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
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

std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    if (wire.funding_rate.empty()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = to_e9(wire.funding_rate);
    // Mark price is exempt from the exact-step rule: binance computes it as an
    // index-weighted average and does not round it to the trading tick, so
    // demanding divisibility would drop every frame.
    if (const auto mark = parse_decimal_e9(wire.mark_price);
        mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    out.interval_s = std::visit(
        [](const auto& concrete) -> std::uint32_t {
            if constexpr (requires { concrete.funding_interval_s; }) {
                return concrete.funding_interval_s;
            } else {
                return 0;
            }
        },
        *instrument);
    out.ts_event = ms_to_unix(wire.event_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (wire.status != "TRADING") {
        return std::nullopt; // SETTLING, PENDING_TRADING, CLOSE: not tradable now
    }
    // contractType distinguishes the perpetual from quarterlies and from the
    // tradfi-hours contracts that share the endpoint. Asking for perps and
    // getting a dated future back is a whole category of position bug.
    if (market == MarketType::Perp && wire.contract_type != "PERPETUAL") {
        return std::nullopt;
    }

    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.step_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Binance, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = to_e9(wire.min_notional);
    base.contract_value = *parse_decimal_e9("1"); // USD-M perps quote in base units
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }
    // exchangeInfo does not carry the funding interval; most USD-M contracts
    // settle every 8h but a growing set settles every 4h, and the authority is
    // /fapi/v1/fundingInfo. Until that pull lands this is the default, and the
    // funding stream's own nextFundingTime is what the strategy should trust.
    return PerpInstrument{base, /*funding_interval_s=*/28'800};
}

} // namespace raikiri::venues::binance
