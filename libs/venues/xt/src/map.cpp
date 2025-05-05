#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/xt/map.hpp>

#include <optional>

namespace raikiri::venues::xt {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means XT changed an instrument's precision, so the message is
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

std::optional<FixedE9> positive_e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw <= 0) {
        return std::nullopt;
    }
    return *value;
}

std::optional<std::int64_t> pow10(std::int64_t n) noexcept {
    if (n < 0 || n > 18) {
        return std::nullopt;
    }
    std::int64_t out = 1;
    for (std::int64_t i = 0; i < n; ++i) {
        out *= 10;
    }
    return out;
}

const InstrumentBase* base_for(std::string_view symbol, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

// Top of one side of a snapshot. XT sorts bids descending and asks ascending
// (verified across 528 frames), so element zero is the touch.
const WireLevel* top(std::span<const WireLevel> side) noexcept {
    return side.empty() ? nullptr : &side.front();
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_from_precision(std::int64_t digits) noexcept {
    if (digits < 0 || digits > 9) {
        return std::nullopt;
    }
    const auto scale = pow10(digits);
    if (!scale.has_value()) {
        return std::nullopt;
    }
    return FixedE9::from_raw(k_e9 / *scale);
}

std::optional<Bbo>
map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto* best_bid = top(wire.bids);
    const auto* best_ask = top(wire.asks);
    if (best_bid == nullptr || best_ask == nullptr) {
        return std::nullopt;
    }
    const auto bid = to_ticks(best_bid->price, *base);
    const auto ask = to_ticks(best_ask->price, *base);
    const auto bid_qty = to_lots(best_bid->qty, *base);
    const auto ask_qty = to_lots(best_ask->qty, *base);
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
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    BookSnapshot out{};
    out.instrument = base->id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    out.venue_seq = wire.update_id;

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
            // the book here rather than being rounded into a price XT never
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
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto rate = parse_decimal_e9(wire.rate_token);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base->id;
    out.rate = *rate;
    out.next_funding_time = ms_to_unix(wire.next_collection_ms, UnixNanos{0});
    if (wire.interval_h > 0 && wire.interval_h <= 24) {
        out.interval_s = static_cast<std::uint32_t>(wire.interval_h * 3600);
    }
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.product_type != "perpetual") {
        return std::nullopt;
    }
    if (wire.state != 0 || !wire.trade_switch || !wire.open_api) {
        return std::nullopt;
    }

    const auto tick = positive_e9(wire.tick_size);
    const auto lot = step_from_precision(wire.qty_precision);
    const auto contract_size = positive_e9(wire.contract_size);
    if (!tick.has_value() || !lot.has_value() || !contract_size.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Xt, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min_notional = positive_e9(wire.min_notional); min_notional.has_value()) {
        base.min_notional = *min_notional;
    }
    base.contract_value = *contract_size;
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::xt
