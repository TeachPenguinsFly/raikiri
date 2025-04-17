#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>

#include <optional>

namespace raikiri::venues::hyperliquid {

namespace {

// A PERP price is capped at 6 - szDecimals decimal places; spot uses 8, which
// is why this is named for its market rather than being a bare 6.
constexpr std::int64_t k_perp_price_digits = 6;

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means the venue changed an instrument's precision, so the
// message is dropped rather than rounded — rounding is how a mark price ends
// up one tick away from the one the venue published.
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

// Venue-scale, not instrument-scaled: rates stay e9 rather than becoming ticks.
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

std::optional<FixedE9> step_from_decimals(std::int64_t decimals) noexcept {
    // FixedE9 holds nine fractional digits; finer has no representation.
    if (decimals < 0 || decimals > 9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::int64_t i = 0; i < decimals; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.bid_price, base);
    const auto ask = to_ticks(wire.ask_price, base);
    const auto bid_qty = to_lots(wire.bid_qty, base);
    const auto ask_qty = to_lots(wire.ask_qty, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt; // includes a side the venue sent as JSON null
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.coin);
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
map_funding(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.coin);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding);
    if (!rate.has_value()) {
        return std::nullopt; // no funding quoted: not the same as zero
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    // predicted_rate stays undefined: no forecast is published, and a zero here
    // would read as a real prediction of no funding.
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    // next_funding_time stays undefined: the boundary is knowable from a wall
    // clock, but the venue states none on either transport, and filling one in
    // would present a derivation as a venue fact. interval_s says the schedule.
    out.interval_s = k_funding_interval_s;
    // No timestamp on this payload at all, so ingest is the only honest one.
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireAsset& wire, MarketType market) {
    // A delisted asset keeps its slot forever (55 of 232), still quoting a
    // stale prevDayPx; loading one admits a symbol no book will ever update.
    if (wire.delisted) {
        return std::nullopt;
    }
    // Spot exists but under a different catalogue call and naming scheme
    // (`@1`, `PURR/USDC`), so a spot request here would silently load perps.
    if (market != MarketType::Perp) {
        return std::nullopt;
    }

    // DERIVED, NOT VENUE-STATED. The catalogue publishes szDecimals and no
    // steps at all, so both come from it as exact powers of ten: size by
    // 10^-szDecimals, price by 10^-(6 - szDecimals), the cap on decimal places.
    //
    // The OTHER rule, at most 5 significant figures, is deliberately NOT a step
    // here: it constrains what may be SUBMITTED, and being magnitude-dependent
    // (spacing 1.0 at 64143, 0.1 at 6414.3) it describes no fixed grid. Nor do
    // published prices obey it — across all 232 assets every markPx, midPx,
    // oraclePx and prevDayPx honours the decimal cap while 45 exceed 5 sig figs
    // ("1.356952", "64145.5"). A sig-fig tick would drop those under exact
    // division; this one never drops a price the venue published.
    const auto lot = step_from_decimals(wire.sz_decimals);
    const auto tick = step_from_decimals(k_perp_price_digits - wire.sz_decimals);
    if (!lot.has_value() || !tick.has_value()) {
        return std::nullopt;
    }
    // Bare ticker: "BTC", not "BTC-USDT-SWAP". Every perp is USDC-margined.
    const auto symbol = SymbolText::from(wire.name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Hyperliquid, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    // Size is the coin itself, so there is no contract multiplier to load.
    base.contract_value = *parse_decimal_e9("1");
    // min_notional stays undefined: the venue's floor is $10 of notional, a
    // quote-currency amount this base-unit field cannot express. A zero would
    // read as "no minimum".
    return PerpInstrument{base, k_funding_interval_s};
}

} // namespace raikiri::venues::hyperliquid
