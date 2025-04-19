#include <raikiri/common/int128.hpp>
#include <raikiri/venues/kraken/map.hpp>

#include <limits>
#include <optional>

namespace raikiri::venues::kraken {

namespace {

// Guards the int128 accumulator and the power-of-ten loops below. No live
// Kraken token comes close: the longest observed mantissa is 17 digits and
// the largest exponent shift is 10.
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

// Splits a JSON numeric token into an exact mantissa and a base-10 exponent.
// Kraken spells one field both ways, so reading only plain decimals silently
// loses whichever frames took the other form.
std::optional<Decomposed> decompose(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    Decomposed out{};
    std::size_t i = 0;
    if (text[i] == '+' || text[i] == '-') {
        out.negative = text[i] == '-';
        ++i;
    }
    int digits = 0;
    int fractional = 0;
    bool seen_digit = false;
    bool seen_dot = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.') {
            if (seen_dot) {
                return std::nullopt;
            }
            seen_dot = true;
            continue;
        }
        if (c == 'e' || c == 'E') {
            break;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        seen_digit = true;
        // Leading zeros carry no significance and must not consume the
        // digit budget: "0.00000000001" is eleven characters of nothing.
        if (out.mantissa != 0 || c != '0') {
            if (++digits > k_max_mantissa_digits) {
                return std::nullopt;
            }
            out.mantissa = out.mantissa * 10 + (c - '0');
        }
        if (seen_dot) {
            ++fractional;
        }
    }
    if (!seen_digit) {
        return std::nullopt;
    }
    out.exponent = -fractional;

    if (i < text.size()) {
        ++i; // skip the 'e'
        bool exponent_negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exponent_negative = text[i] == '-';
            ++i;
        }
        if (i >= text.size()) {
            return std::nullopt;
        }
        int value = 0;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
            if (value > k_max_shift * 4) {
                return std::nullopt;
            }
        }
        out.exponent += exponent_negative ? -value : value;
    }
    return out;
}

// mantissa * 10^exponent -> 1e9 fixed point. `exact` decides what happens to
// significance below 1e-9: refuse it, or truncate toward zero.
std::optional<FixedE9> scale_to_e9(const Decomposed& parts, bool exact) noexcept {
    int shift = parts.exponent + 9;
    if (shift > k_max_shift || shift < -k_max_shift) {
        return std::nullopt;
    }
    int128_t value = parts.mantissa;
    for (; shift > 0; --shift) {
        value *= 10;
    }
    for (; shift < 0; ++shift) {
        if (exact && value % 10 != 0) {
            return std::nullopt;
        }
        value /= 10; // int128 division truncates toward zero, which is what we want
    }
    if (parts.negative) {
        value = -value;
    }
    constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
    if (value > k_i64_max || value < -k_i64_max) {
        return std::nullopt;
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(value));
}

// Rates only. relative_funding_rate is a full-precision quotient (1.477885e-5
// is eleven decimals), so an exact read would refuse every funding frame
// Kraken has ever sent and present a live stream as silence. Truncating a
// dimensionless RATE at 1e-9 leaves eight significant figures of a number
// nothing divides an order by; prices and quantities stay exact.
std::optional<FixedE9> to_rate_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means Kraken changed an instrument's precision, so the
// message is dropped rather than rounded — rounding is how an order ends up
// one tick away from the intended price.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(text);
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

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/true) : std::nullopt;
}

std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept {
    // Nine is where 1e9 fixed point runs out on one side and where 1e9 * 10^9
    // still fits int64 on the other; live rows span only -3..4.
    if (precision > 9 || precision < -9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::int64_t i = 0; i < precision; ++i) {
        raw /= 10;
    }
    for (std::int64_t i = precision; i < 0; ++i) {
        raw *= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.suspended) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.bid, base);
    const auto ask = to_ticks(wire.ask, base);
    const auto bid_qty = to_lots(wire.bid_size, base);
    const auto ask_qty = to_lots(wire.ask_size, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }
    // Kraken quotes an empty side as a literal zero, and zero divides by any
    // tick, so "no quote" has to be caught here or it publishes as a price.
    if (bid->raw <= 0 || ask->raw <= 0 || bid_qty->raw <= 0 || ask_qty->raw <= 0) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    if (!price.has_value() || !qty.has_value() || price->raw <= 0 || qty->raw <= 0) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    // A liquidation print is real volume at a real price and is kept.
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_rate_e9(wire.relative_funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // ticker_lite quotes none: not the same as zero
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto predicted = to_rate_e9(wire.relative_funding_rate_prediction);
        predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    // markPrice is index-derived, not tradeable, so it lands on a tick only
    // by coincidence (64113.93588534861 against a tick of 1). Attempted, and
    // left undefined when it does not divide: rounding would publish a mark
    // that never existed.
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_rate_time_ms, UnixNanos{0});
    // interval_s stays zero: Kraken publishes only the NEXT settlement time,
    // never the period, and fundingRateCoefficient (8 on every live row)
    // scales the rate rather than naming an interval. A guessed 8h would be
    // invisible when wrong.
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (!wire.tradeable) {
        return std::nullopt;
    }
    // Kraken Spot is a different host, symbology and message shape; this
    // adapter serves Kraken Futures only, so a spot request is declined here
    // rather than answered with futures rows.
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    // `type` does NOT separate perpetual from dated — flexible_futures covers
    // both PF_ and FF_, futures_inverse both PI_ and FI_. Getting a dated
    // contract back when asking for perps is a position bug, so the venue's
    // own marker and the symbol prefix both have to agree.
    if (!wire.last_trading_time.empty()) {
        return std::nullopt;
    }
    const bool linear = wire.symbol.starts_with("PF_");
    const bool inverse = wire.symbol.starts_with("PI_");
    if (!linear && !inverse) {
        return std::nullopt;
    }
    if (!wire.has_precision) {
        return std::nullopt; // an absent exponent is not a precision of zero
    }

    // KNOWN LIMIT, not a venue quirk: FixedE9 holds nine fractional digits
    // and PF_PEPEUSD ticks at 0.0000000001. Refused here, correctly under the
    // no-rounding rule; reaching it means widening the domain's fixed-point
    // scale. Two rows of 298 today.
    const auto tick = parse_number_e9(wire.tick_size);
    const auto lot = step_from_precision(wire.contract_value_trade_precision);
    const auto contract = parse_number_e9(wire.contract_size);
    if (!tick.has_value() || tick->raw <= 0 || !lot.has_value() || !contract.has_value() ||
        contract->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Kraken, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    // contractSize is 1 on every live row, but the UNIT behind it differs by
    // family and the domain records it nowhere: a PF_ quantity is base coin
    // (0.0015 BTC), a PI_ quantity is USD of notional (55146 USD). So
    // qty * contract_value is a notional for PF_ and meaningless for PI_ —
    // which is why the two never share a symbol.
    base.contract_value = *contract;
    // min_notional stays undefined: Kraken's floor is a contract count and
    // converting it needs a price we do not have. Zero would read as "no
    // minimum", which is the wrong answer.
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::kraken
