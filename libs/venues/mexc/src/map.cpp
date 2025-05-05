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
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::mexc
