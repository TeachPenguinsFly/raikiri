#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/venues/bitmex/map.hpp>

#include <charconv>
#include <limits>
#include <optional>

namespace raikiri::venues::bitmex {

namespace {

// BitMEX's two serialisers disagree about small numbers: REST writes
// "fundingRate":0.0001, the websocket writes 1.0E-4 for the identical field.
// parse_decimal_e9 speaks only the plain form, so the exponent is applied here
// by moving the decimal point in integers — keeping the venue's exact digits is
// the whole reason the wire layer hands over a raw token.
std::optional<FixedE9> parse_number_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto split = text.find_first_of("eE");
    if (split == std::string_view::npos) {
        const auto plain = parse_decimal_e9(text);
        return plain.has_value() ? std::optional{*plain} : std::nullopt;
    }
    const auto mantissa = parse_decimal_e9(text.substr(0, split));
    if (!mantissa.has_value()) {
        return std::nullopt;
    }
    auto exponent_text = text.substr(split + 1);
    bool negative = false;
    if (!exponent_text.empty() && (exponent_text.front() == '+' || exponent_text.front() == '-')) {
        negative = exponent_text.front() == '-';
        exponent_text.remove_prefix(1);
    }
    std::int64_t exponent = 0;
    const auto* const end = exponent_text.data() + exponent_text.size();
    const auto [stop, ec] = std::from_chars(exponent_text.data(), end, exponent);
    if (ec != std::errc{} || stop != end || exponent > 18) {
        return std::nullopt;
    }
    std::int64_t power = 1;
    for (std::int64_t i = 0; i < exponent; ++i) {
        power *= 10;
    }
    if (!negative) {
        return mantissa->scaled_by(power);
    }
    if (mantissa->raw % power != 0) {
        return std::nullopt;
    }
    return FixedE9::from_raw(mantissa->raw / power);
}

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means BitMEX changed a precision, so the message is dropped
// rather than rounded — rounding puts a reference price a tick off the venue's.
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

// Rates and fees are venue-scale decimals, not instrument-scaled, so they keep
// their 1e9 representation rather than becoming ticks.
std::optional<FixedE9> to_e9(std::string_view text) {
    return parse_number_e9(text);
}

// `multiplier` and the three *Multiplier columns are counts; a fractional one
// would mean the contract size is not an exact multiple of anything.
std::optional<std::int64_t> whole_number(std::string_view text) {
    const auto value = parse_number_e9(text);
    if (!value.has_value() || value->raw % k_e9 != 0) {
        return std::nullopt;
    }
    return value->raw / k_e9;
}

std::int64_t magnitude(std::int64_t value) noexcept {
    return value < 0 ? -value : value;
}

// numerator / denominator as an e9 decimal, exactly or not at all. 128-bit
// because a scale column of 1e9 scaled by another 1e9 leaves int64 nothing to
// spare, and an overflow here would be a silently wrong contract size.
std::optional<FixedE9> exact_ratio(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0) {
        return std::nullopt;
    }
    const int128_t scaled = static_cast<int128_t>(numerator) * k_e9;
    if (scaled % denominator != 0) {
        return std::nullopt;
    }
    const int128_t out = scaled / denominator;
    if (out <= 0 || out > static_cast<int128_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(out));
}

// BitMEX ships every time as ISO-8601 text, so the choice was a date library, a
// double, or twenty lines of integer arithmetic over a fixed-width form. The
// form is genuinely fixed — "2026-08-10T20:53:31.008Z", 24 characters, on every
// timestamp in the captures — so anything else is refused, not half-understood.
constexpr std::size_t k_iso_len = 24;

std::optional<std::int64_t> digits_at(std::string_view text, std::size_t offset, std::size_t len) {
    std::int64_t out = 0;
    const auto* const begin = text.data() + offset;
    const auto* const end = begin + len;
    const auto [stop, ec] = std::from_chars(begin, end, out);
    return (ec == std::errc{} && stop == end) ? std::optional{out} : std::nullopt;
}

// Days since 1970-01-01 from a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Integer throughout, which is the whole point.
std::int64_t days_from_civil(std::int64_t year, std::int64_t month, std::int64_t day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t yoe = year - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct IsoParts {
    std::int64_t year{0};
    std::int64_t month{0};
    std::int64_t day{0};
    std::int64_t hour{0};
    std::int64_t minute{0};
    std::int64_t second{0};
    std::int64_t milli{0};
};

std::optional<IsoParts> split_iso8601(std::string_view text) noexcept {
    if (text.size() != k_iso_len || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != '.' || text[23] != 'Z') {
        return std::nullopt;
    }
    IsoParts parts{};
    const auto year = digits_at(text, 0, 4);
    const auto month = digits_at(text, 5, 2);
    const auto day = digits_at(text, 8, 2);
    const auto hour = digits_at(text, 11, 2);
    const auto minute = digits_at(text, 14, 2);
    const auto second = digits_at(text, 17, 2);
    const auto milli = digits_at(text, 20, 3);
    if (!year || !month || !day || !hour || !minute || !second || !milli) {
        return std::nullopt;
    }
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 ||
        *second > 60) {
        return std::nullopt;
    }
    parts = {*year, *month, *day, *hour, *minute, *second, *milli};
    return parts;
}

// The `typ` column is a CFI-style code and two of them mean "perpetual":
// FFWCSX crypto, FFSCSX FX/commodity. IFXXXP is spot, FFCCSX the dated future.
std::optional<MarketType> market_of_typ(std::string_view typ) noexcept {
    if (typ == "FFWCSX" || typ == "FFSCSX") {
        return MarketType::Perp;
    }
    if (typ == "FFCCSX") {
        return MarketType::Future;
    }
    if (typ == "IFXXXP") {
        return MarketType::Spot;
    }
    return std::nullopt;
}

// One contract's size, and the reason this adapter exists.
//
// READ THE DIRECTION CAREFULLY, because nothing downstream can catch it being
// backwards. On an INVERSE row the answer is denominated in the QUOTE currency;
// on a linear or spot row it is denominated in the BASE currency. XBTUSD is
// quoted in USD, settled in BTC, and one contract is worth 1 USD — so its
// contract_value is 1 (USD per contract), NOT 1/price (BTC per contract). The
// BTC side of an inverse position is the side that floats with price, and a
// floating quantity is not something an instrument row can hold.
//
// BitMEX states the direction twice: isInverse, and a NEGATIVE `multiplier`
// (-100000000 on XBTUSD). The sign is cross-checked but never stored.
//
//   inverse  |multiplier| / |underlyingToSettleMultiplier|  -> quote per contract
//   linear    multiplier  /  quoteToSettleMultiplier        -> base  per contract
//   spot      1           /  underlyingToPositionMultiplier -> base  per size unit
//
// All three verified against the venue's own 24h aggregates, not the docs: on
// XBTUSD foreignNotional24h / volume24h is exactly 1 (USD per contract) while
// homeNotional24h / volume24h is 1.55e-5 — that is 1/price, not a constant of
// the contract. XBTUSDT gives 1e-6 BTC per contract the other way, and a live
// spot print of size 70000 carried homeNotional 0.0007 BTC: 1e-8 per unit.
std::optional<FixedE9> contract_value_of(const WireInstrument& wire, MarketType market) {
    const auto multiplier = whole_number(wire.multiplier);
    if (!multiplier.has_value() || *multiplier == 0) {
        return std::nullopt;
    }
    if ((*multiplier < 0) != wire.inverse) {
        return std::nullopt;
    }
    if (wire.inverse) {
        const auto scale = whole_number(wire.underlying_to_settle);
        if (!scale.has_value()) {
            return std::nullopt;
        }
        return exact_ratio(magnitude(*multiplier), magnitude(*scale));
    }
    if (market == MarketType::Spot) {
        const auto scale = whole_number(wire.underlying_to_position);
        if (!scale.has_value()) {
            return std::nullopt;
        }
        return exact_ratio(1, *scale);
    }
    const auto scale = whole_number(wire.quote_to_settle);
    if (!scale.has_value()) {
        return std::nullopt;
    }
    return exact_ratio(*multiplier, *scale);
}

} // namespace

UnixNanos iso8601_to_unix(std::string_view text, UnixNanos fallback) noexcept {
    const auto parts = split_iso8601(text);
    if (!parts.has_value()) {
        return fallback;
    }
    const auto days = days_from_civil(parts->year, parts->month, parts->day);
    const auto seconds = days * 86'400 + parts->hour * 3'600 + parts->minute * 60 + parts->second;
    if (seconds < 0) {
        return fallback;
    }
    return UnixNanos{static_cast<std::uint64_t>(seconds * 1'000 + parts->milli) * 1'000'000ULL};
}

std::uint32_t funding_interval_seconds(std::string_view text) noexcept {
    const auto parts = split_iso8601(text);
    if (!parts.has_value()) {
        return 0;
    }
    return static_cast<std::uint32_t>(parts->hour * 3'600 + parts->minute * 60 + parts->second);
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = iso8601_to_unix(wire.ts, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Bbo>
QuoteBook::apply(const WireQuote& wire, const InstrumentBase& base, UnixNanos ts_init) {
    PoolTop* slot = nullptr;
    PoolTop* spare = nullptr;
    for (auto& pool : pools_) {
        if (pool.used) {
            if (pool.pool.view() == wire.pool) {
                slot = &pool;
                break;
            }
        } else if (spare == nullptr) {
            spare = &pool;
        }
    }
    if (slot == nullptr) {
        const auto name = FixedString<16>::from(wire.pool);
        if (spare == nullptr || !name.has_value()) {
            return std::nullopt;
        }
        spare->pool = *name;
        spare->used = true;
        slot = spare;
    }

    if (!wire.bid_price.empty()) {
        const auto price = to_ticks(wire.bid_price, base);
        const auto qty = to_lots(wire.bid_qty, base);
        if (!price.has_value() || !qty.has_value()) {
            return std::nullopt;
        }
        slot->bid = *price;
        slot->bid_qty = *qty;
    }
    if (!wire.ask_price.empty()) {
        const auto price = to_ticks(wire.ask_price, base);
        const auto qty = to_lots(wire.ask_qty, base);
        if (!price.has_value() || !qty.has_value()) {
            return std::nullopt;
        }
        slot->ask = *price;
        slot->ask_qty = *qty;
    }

    Bbo out{};
    out.instrument = base.id;
    std::int64_t bid_qty = 0;
    std::int64_t ask_qty = 0;
    for (const auto& pool : pools_) {
        if (!pool.used) {
            continue;
        }
        if (!pool.bid.is_undef()) {
            if (out.bid.is_undef() || pool.bid.raw > out.bid.raw) {
                out.bid = pool.bid;
                bid_qty = pool.bid_qty.raw;
            } else if (pool.bid.raw == out.bid.raw) {
                bid_qty += pool.bid_qty.raw;
            }
        }
        if (!pool.ask.is_undef()) {
            if (out.ask.is_undef() || pool.ask.raw < out.ask.raw) {
                out.ask = pool.ask;
                ask_qty = pool.ask_qty.raw;
            } else if (pool.ask.raw == out.ask.raw) {
                ask_qty += pool.ask_qty.raw;
            }
        }
    }
    if (out.bid.is_undef() || out.ask.is_undef()) {
        return std::nullopt;
    }
    out.bid_qty = QtyLots::from_raw(bid_qty);
    out.ask_qty = QtyLots::from_raw(ask_qty);
    out.ts_event = iso8601_to_unix(wire.ts, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> InstrumentState::apply(const WireInstrumentTick& wire,
                                                    const InstrumentBase& base,
                                                    UnixNanos ts_init) {
    if (const auto rate = to_e9(wire.funding_rate); rate.has_value()) {
        rate_ = *rate;
    }
    if (const auto predicted = to_e9(wire.indicative_funding_rate); predicted.has_value()) {
        predicted_ = *predicted;
    }
    if (const auto next = iso8601_to_unix(wire.funding_ts, UnixNanos{0}); next.ns != 0) {
        next_funding_ = next;
    }
    if (const auto interval = funding_interval_seconds(wire.funding_interval); interval != 0) {
        interval_s_ = interval;
    }
    const bool carries_funding = !wire.funding_rate.empty() ||
                                 !wire.indicative_funding_rate.empty() ||
                                 !wire.funding_ts.empty() || !wire.funding_interval.empty();
    if (!carries_funding || rate_.is_undef()) {
        return std::nullopt;
    }

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = rate_;
    out.predicted_rate = predicted_;
    out.next_funding_time = next_funding_;
    out.interval_s = interval_s_;
    out.ts_event = iso8601_to_unix(wire.ts, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (wire.state != "Open") {
        return std::nullopt;
    }
    if (wire.quanto) {
        return std::nullopt;
    }
    const auto typ_market = market_of_typ(wire.typ);
    if (!typ_market.has_value() || *typ_market != market) {
        return std::nullopt;
    }
    const bool funds = !wire.funding_interval.empty();
    if (market == MarketType::Perp && !funds) {
        return std::nullopt;
    }
    if (market == MarketType::Future && funds) {
        return std::nullopt;
    }

    const auto tick = parse_number_e9(wire.tick_size);
    const auto lot = parse_number_e9(wire.lot_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto value = contract_value_of(wire, market);
    if (!value.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Bitmex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *value;
    // Stated per row, so no hardcoded pair is needed here.
    if (const auto maker = to_e9(wire.maker_fee); maker.has_value()) {
        base.maker_fee = *maker;
    }
    if (const auto taker = to_e9(wire.taker_fee); taker.has_value()) {
        base.taker_fee = *taker;
    }

    switch (market) {
    case MarketType::Spot:
        return SpotInstrument{base};
    case MarketType::Perp:
        return PerpInstrument{base, funding_interval_seconds(wire.funding_interval)};
    case MarketType::Future: {
        const auto expiry = iso8601_to_unix(wire.expiry, UnixNanos{0});
        if (expiry.ns == 0) {
            return std::nullopt; // a dated contract without a readable date
        }
        return FutureInstrument{base, expiry};
    }
    case MarketType::Option:
        break;
    }
    return std::nullopt;
}

} // namespace raikiri::venues::bitmex
