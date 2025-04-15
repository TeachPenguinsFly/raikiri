#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/gate/map.hpp>

#include <optional>

namespace raikiri::venues::gate {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means Gate changed an instrument's precision, so the message is
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

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

UnixNanos s_to_unix(std::uint64_t seconds) noexcept {
    return UnixNanos{seconds * 1'000'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
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
    // `t` is the matching-engine time in milliseconds; the frame's outer
    // `time_ms` is when the gateway sent it, and using that would fold Gate's
    // own fan-out delay into every latency measurement.
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
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
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    // predicted_rate stays undefined: funding_rate_indicative is the only
    // candidate, Gate's SDK marks it deprecated in favour of funding_rate, and
    // it equalled funding_rate on every observed frame.
    //
    // Mark is optional rather than required: Gate quotes it on
    // mark_price_round, a finer grid than order_price_round on some contracts,
    // so a mark off the order grid is normal rather than a schema event — and
    // funding is the datum this update exists for.
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = s_to_unix(wire.funding_next_apply_s);
    out.interval_s = wire.funding_interval_s;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    // Every USDT-settled contract here is a perpetual; Gate keeps dated
    // futures behind /delivery and spot behind a differently shaped catalogue,
    // so neither can arrive on this path.
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    // in_delisting stays true through the reduce-only window while the row
    // still looks tradable, and `status` covers prelaunch and circuit_breaker.
    // Both were uniformly benign in the 899-row capture; they are the venue's
    // own liveness statements and refusing on them is cheaper than discovering
    // at order time that a contract stopped accepting opens.
    if (wire.in_delisting || wire.status != "trading") {
        return std::nullopt;
    }

    // KNOWN LIMIT, not a venue quirk: FixedE9 carries nine fractional digits
    // and Gate lists CHEEMS_USDT, MOG_USDT and SATS_USDT at ticks down to
    // 1e-11. Refusing them is correct under the no-rounding rule; reaching
    // them means widening the fixed-point scale across the domain. Three of
    // 899 today, and okx refuses SATS for the same reason.
    const auto tick = parse_decimal_e9(wire.order_price_round);
    if (!tick.has_value() || tick->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Gate, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    // One whole contract. Gate publishes no size step: `order_size_min` is a
    // floor (0 on the 15 contracts flagged enable_decimal), not a step. Public
    // market data quotes integer contract counts everywhere, enable_decimal
    // included — verified live on ETH_USDT and SOL_USDT, which carry the flag.
    // That flag governs what an ORDER may specify, so it becomes a real
    // question when order entry lands, not before.
    base.lot_size = FixedE9::from_raw(k_e9);
    // min_notional stays undefined: Gate's floor is a count of CONTRACTS, and
    // converting it to a notional needs a price we do not have here. A zero
    // would read as "no minimum", which is the wrong answer.
    //
    // One contract is quanto_multiplier of the base asset (0.0001 BTC on
    // BTC_USDT). Binance's USD-M perps quote in base units and can pin this to
    // 1; Gate cannot, and a hardcoded 1 would misprice every BTC_USDT notional
    // by 10000x.
    const auto quanto = parse_decimal_e9(wire.quanto_multiplier);
    if (!quanto.has_value() || quanto->raw <= 0) {
        return std::nullopt;
    }
    base.contract_value = *quanto;
    return PerpInstrument{base, wire.funding_interval_s};
}

} // namespace raikiri::venues::gate
