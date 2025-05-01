#include <raikiri/common/int128.hpp>
#include <raikiri/venues/bitfinex/map.hpp>

#include <limits>
#include <optional>
#include <string>

namespace raikiri::venues::bitfinex {

namespace {

// Guards the int128 accumulator and the power-of-ten loops below. No live
// Bitfinex token comes close: the longest observed mantissa is 14 digits and
// the largest exponent shift is 8.
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

constexpr std::string_view k_symbol_prefix = "t";

// Bitfinex serves its PAPER-TRADING pairs from the same production /conf
// endpoint as the real ones, under a TEST prefix on both legs
// (TESTBTC:TESTUSD, TESTBTCF0:TESTUSDTF0 — 28 of 272 rows). They quote
// simulated prices and are reachable only from a paper sub-account, so
// admitting one puts a fabricated book in a live catalogue.
constexpr std::string_view k_paper_prefix = "TEST";

// Futures pairs are F0-suffixed on both legs (BTCF0:USTF0, ETHF0:BTCF0);
// no spot pair contains F0 at all, across all 272 rows.
constexpr std::string_view k_futures_marker = "F0";

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

// Splits a JSON numeric token into an exact mantissa and a base-10 exponent.
// Bitfinex spells the same field both ways — a trades frame carried 0.00011
// and 1e-8 in the same payload — so reading only plain decimals silently loses
// whichever prints took the other form.
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

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means Bitfinex published finer precision than the derived
// 1e-8 grid, so the message is dropped rather than rounded — rounding is how
// an order ends up one tick away from the intended price.
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

// Rates are venue-scale decimals, not instrument-scaled, so they keep their
// 1e9 representation rather than becoming ticks. Truncating below 1e-9 leaves
// eight significant figures of a dimensionless number nothing divides an order
// by; prices and quantities stay exact.
std::optional<FixedE9> to_rate_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

// A trade's AMOUNT and a book level's AMOUNT are both signed, and in neither
// case is the sign part of the quantity. Splitting it here keeps a negative
// number from ever reaching QtyLots.
struct SignedToken {
    std::string_view magnitude;
    bool negative{false};
};

SignedToken split_sign(std::string_view text) noexcept {
    SignedToken out{.magnitude = text, .negative = false};
    if (!out.magnitude.empty() && (out.magnitude.front() == '-' || out.magnitude.front() == '+')) {
        out.negative = out.magnitude.front() == '-';
        out.magnitude.remove_prefix(1);
    }
    return out;
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

std::optional<Bbo> map_bbo(const WireTicker& wire,
                           std::string_view symbol,
                           const exec::InstrumentTable& table,
                           UnixNanos ts_event,
                           UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
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
    if (bid->raw <= 0 || ask->raw <= 0 || bid_qty->raw <= 0 || ask_qty->raw <= 0 ||
        bid->raw >= ask->raw) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ts_event.ns == 0 ? ts_init : ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire,
                                   std::string_view symbol,
                                   const exec::InstrumentTable& table,
                                   UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto amount = split_sign(wire.amount);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(amount.magnitude, base);
    if (!price.has_value() || !qty.has_value() || price->raw <= 0 || qty->raw <= 0) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = amount.negative ? Side::Sell : Side::Buy;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireDerivStatus& wire,
                                         std::string_view symbol,
                                         const exec::InstrumentTable& table,
                                         UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_rate_e9(wire.current_funding);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto predicted = to_rate_e9(wire.next_funding_accrued); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    out.interval_s = k_funding_interval_s;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WirePairInfo& wire, MarketType market) {
    if (wire.pair.starts_with(k_paper_prefix)) {
        return std::nullopt;
    }
    const bool futures = wire.pair.find(k_futures_marker) != std::string_view::npos;
    if (market == MarketType::Spot) {
        if (futures) {
            return std::nullopt;
        }
    } else if (market == MarketType::Perp) {
        if (!futures) {
            return std::nullopt;
        }
    } else {
        return std::nullopt; // dated futures and options are not listed here
    }
    const auto min_size = parse_number_e9(wire.min_order_size);
    if (!min_size.has_value() || min_size->raw <= 0) {
        return std::nullopt;
    }

    std::string wire_symbol;
    wire_symbol.reserve(k_symbol_prefix.size() + wire.pair.size());
    wire_symbol.append(k_symbol_prefix).append(wire.pair);
    const auto symbol = SymbolText::from(wire_symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Bitfinex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = FixedE9::from_raw(k_step_e9);
    base.lot_size = FixedE9::from_raw(k_step_e9);
    base.contract_value = *parse_decimal_e9("1");
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }
    return PerpInstrument{base, k_funding_interval_s};
}

} // namespace raikiri::venues::bitfinex
