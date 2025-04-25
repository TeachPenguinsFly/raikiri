#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/venues/htx/map.hpp>

#include <limits>
#include <optional>
#include <variant>

namespace raikiri::venues::htx {

namespace {

// 1 is listed and tradable; 3 is a row on its way out that still ships usable
// numbers, so only the status keeps it out.
constexpr std::int64_t k_contract_listed = 1;

// Guards the int128 accumulator and the power-of-ten loops; no live HTX token
// comes close.
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

constexpr int k_seconds_per_hour = 3600;

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

// Splits a JSON numeric token into an exact mantissa and a base-10 exponent.
// HTX spells price_tick both ways within ONE response, so reading only plain
// decimals would silently lose whichever rows took the other form.
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
        // Leading zeros carry no significance and must not consume the digit
        // budget: "0.000000000001" is twelve characters of nothing.
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
        value /= 10;
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

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means HTX changed an instrument's precision, so the message is
// dropped rather than rounded — rounding is how an order ends up one tick away
// from the intended price.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = exact_e9(text);
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

std::optional<FixedE9> exact_e9(std::string_view token) {
    if (token.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(token);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/true) : std::nullopt;
}

std::optional<FixedE9> rate_e9(std::string_view token) {
    if (token.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(token);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
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
    // tick.ts, not the frame's own: the first is when the book changed and
    // the second is when HTX got round to sending it.
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    // `amount`, not `quantity`: bbo and depth quote contracts, and mixing the
    // two units within one instrument's series is a silent factor error.
    const auto qty = to_lots(wire.amount, base);
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
map_funding(const WireFunding& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = rate_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base_of(*instrument).id;
    out.rate = *rate;
    if (const auto predicted = rate_e9(wire.estimated_rate); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    // settlement_time, not funding_time — see wire.hpp. Reading the two the
    // way the REST endpoint names them puts the next settlement a few seconds
    // from now, forever.
    out.next_funding_time = ms_to_unix(wire.settlement_ms, UnixNanos{0});
    if (const auto* perp = std::get_if<PerpInstrument>(instrument); perp != nullptr) {
        out.interval_s = perp->funding_interval_s;
    }
    out.ts_event = ms_to_unix(wire.funding_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    if (wire.contract_status != k_contract_listed) {
        return std::nullopt;
    }
    // /swap_contract_info serves USDT-margined swaps only; HTX puts its dated
    // futures behind swap_api and its spot behind a different host entirely.
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.business_type != "swap" || wire.contract_type != "swap") {
        return std::nullopt;
    }

    // KNOWN LIMIT, not a venue quirk: FixedE9 carries nine fractional digits
    // and PEPE-USDT's tick is 1e-10. Refusing it is correct under the
    // no-rounding rule; reaching it needs a wider scale across the domain. One
    // instrument of 276 today.
    const auto tick = exact_e9(wire.price_tick);
    const auto size = exact_e9(wire.contract_size);
    if (!tick.has_value() || tick->raw <= 0 || !size.has_value() || size->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.contract_code);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Htx, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    // One contract, exactly: HTX names an integer `volume` on an order and
    // there is no finer quantity step to publish, which is why the catalogue
    // has no field for one.
    base.lot_size = FixedE9::from_raw(k_e9);
    // contract_size is what one contract is worth in base units (0.001 BTC on
    // BTC-USDT). Every size on the wire counts contracts, so this is the only
    // factor that turns one into a notional; pinning it to 1 the way a
    // base-unit venue allows would misprice BTC-USDT by a thousand.
    base.contract_value = *size;
    // min_notional stays undefined: this endpoint publishes no per-contract
    // minimum, and a zero would read as "no minimum".
    std::uint32_t interval_s = 0;
    if (wire.settlement_period_h > 0) {
        interval_s = static_cast<std::uint32_t>(wire.settlement_period_h * k_seconds_per_hour);
    }
    return PerpInstrument{base, interval_s};
}

} // namespace raikiri::venues::htx
