#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/bitget/map.hpp>

#include <optional>
#include <variant>

namespace raikiri::venues::bitget {

namespace {

// A value that does not divide evenly means the venue changed an
// instrument's precision, so the message is dropped rather than rounded.
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

// Rates are venue-scale, not instrument-scaled, so they stay at 1e9.
std::optional<FixedE9> to_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? std::optional{*value} : std::nullopt;
}

// Counts. Zero is a legitimate pricePlace (one live row quotes in whole
// units), which is why this accepts it where okx's equivalent does not.
std::optional<std::int64_t> whole_number(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw < 0 || value->raw % k_e9 != 0) {
        return std::nullopt;
    }
    return value->raw / k_e9;
}

// Both price fields matter: `pricePlace` counts decimals and `priceEndStep`
// counts how many of that smallest unit make one tick, so the step is
// priceEndStep * 10^-pricePlace. A row with pricePlace 2 and priceEndStep 5
// moves in 0.05; reading only pricePlace gives it a 0.01 tick, and every
// price then divides exactly at five times the real resolution, so nothing is
// ever dropped to say so. MEASURED 2026-08-11: priceEndStep is "1" on all 745
// USDT-FUTURES rows and all 60 COIN-/USDC-FUTURES rows, which is why the
// factor is easy to drop and impossible to notice until a row changes it.
//
// Integer scale, no double: the domain's fixed point is e9, so a place past
// nine floors the unit to a zero step and is refused rather than clamped.
// PEPEUSDT (pricePlace 10) is the live row that takes this path.
std::optional<FixedE9> tick_size_of(std::string_view place_text, std::string_view end_step_text) {
    const auto place = whole_number(place_text);
    const auto end_step = whole_number(end_step_text);
    if (!place.has_value() || !end_step.has_value() || *end_step <= 0 || *place > 9) {
        return std::nullopt;
    }
    std::int64_t unit = k_e9;
    for (std::int64_t i = 0; i < *place; ++i) {
        unit /= 10;
    }
    std::int64_t raw = 0;
    if (__builtin_mul_overflow(unit, *end_step, &raw)) {
        return std::nullopt;
    }
    return FixedE9::from_raw(raw);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
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
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    if (const auto* perp = std::get_if<PerpInstrument>(instrument); perp != nullptr) {
        out.interval_s = perp->funding_interval_s;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
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

std::optional<BookSnapshot>
map_book(const WireBooks& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!wire.snapshot) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    if (wire.seq_id > 0) {
        out.venue_seq = static_cast<std::uint64_t>(wire.seq_id);
    }

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
            // the book here rather than being rounded into a price Bitget never
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

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    // Spot lives behind a different endpoint with a different row shape.
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.symbol_type != "perpetual") {
        return std::nullopt;
    }
    if (wire.symbol_status != "normal") {
        return std::nullopt;
    }

    const auto tick = tick_size_of(wire.price_place, wire.price_end_step);
    const auto lot = to_e9(wire.size_multiplier);
    if (!tick.has_value() || !lot.has_value() || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Bitget, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min_notional = to_e9(wire.min_trade_usdt); min_notional.has_value()) {
        base.min_notional = *min_notional;
    }
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    std::uint32_t interval_s = 0;
    if (const auto hours = whole_number(wire.fund_interval); hours.has_value()) {
        interval_s = static_cast<std::uint32_t>(*hours * 3600);
    }
    return PerpInstrument{base, interval_s};
}

} // namespace raikiri::venues::bitget
