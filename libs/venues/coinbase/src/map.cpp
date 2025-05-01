#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/coinbase/map.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::coinbase {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means Coinbase changed a product's increments, so the message
// is dropped rather than rounded — rounding is how an order ends up one tick
// away from the intended price.
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

// ---------------------------------------------------------------- time ----
//
// Coinbase stamps everything with an ISO-8601 string and never an epoch
// number, which is unlike every other venue in this tree. /time does publish
// an `epoch` sibling, but as a JSON floating-point number — the one
// representation this codebase refuses to make decisions on — so the string
// is the only usable form and a parser is unavoidable.
//
// Hand-rolled because the alternatives are worse: a date library is a
// dependency added for one venue, and strptime is locale- and TZ-sensitive
// and cannot carry sub-second digits at all. The head is fixed width, so the
// only variable part is the fraction, whose length is 3, 6 or 9 depending on
// which endpoint answered.

constexpr std::size_t k_head_len = 19; // "YYYY-MM-DDTHH:MM:SS"

std::optional<std::int64_t> digits_at(std::string_view text, std::size_t at, std::size_t count) {
    std::int64_t out = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[at + i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        out = out * 10 + (c - '0');
    }
    return out;
}

// Howard Hinnant's civil-from-days inverse: integer arithmetic only, correct
// for the whole proleptic Gregorian calendar, and no <chrono> calendar types
// (C++20 ships them but libstdc++ drags a tzdb in behind them).
constexpr std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
    y -= static_cast<std::int64_t>(m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146'097 + doe - 719'468;
}

// UnixNanos runs out in 2554 and this multiplies by 1e9, so the guard is on
// seconds: anything past it would wrap into a plausible-looking past date.
constexpr std::int64_t k_max_epoch_s = 18'446'744'073LL;

} // namespace

std::optional<UnixNanos> parse_iso8601(std::string_view text) noexcept {
    if (text.size() < k_head_len) {
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto year = digits_at(text, 0, 4);
    const auto month = digits_at(text, 5, 2);
    const auto day = digits_at(text, 8, 2);
    const auto hour = digits_at(text, 11, 2);
    const auto minute = digits_at(text, 14, 2);
    const auto second = digits_at(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 ||
        *second > 59) {
        return std::nullopt;
    }

    std::int64_t frac_ns = 0;
    std::size_t i = k_head_len;
    if (i < text.size() && text[i] == '.') {
        ++i;
        std::int64_t scale = 100'000'000; // the first fractional digit is 1e8 ns
        std::size_t seen = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (scale > 0) {
                frac_ns += static_cast<std::int64_t>(text[i] - '0') * scale;
                scale /= 10;
            }
            ++i;
            ++seen;
        }
        if (seen == 0) {
            return std::nullopt;
        }
    }
    // 'Z' is the only zone Coinbase sends.
    if (i != text.size() && !(text[i] == 'Z' && i + 1 == text.size())) {
        return std::nullopt;
    }

    const std::int64_t secs =
        days_from_civil(*year, *month, *day) * 86'400 + *hour * 3'600 + *minute * 60 + *second;
    if (secs < 0 || secs > k_max_epoch_s) {
        return std::nullopt;
    }
    return UnixNanos{static_cast<std::uint64_t>(secs) * 1'000'000'000ULL +
                     static_cast<std::uint64_t>(frac_ns)};
}

UnixNanos iso_to_unix(std::string_view text, UnixNanos fallback) noexcept {
    const auto parsed = parse_iso8601(text);
    return parsed.has_value() ? *parsed : fallback;
}

std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.best_bid, base);
    const auto ask = to_ticks(wire.best_ask, base);
    const auto bid_qty = to_lots(wire.best_bid_size, base);
    const auto ask_qty = to_lots(wire.best_ask_size, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = iso_to_unix(wire.time, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireMatch& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.size, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    // THE FLIP.
    out.aggressor = wire.maker_sold ? Side::Buy : Side::Sell;
    out.ts_event = iso_to_unix(wire.time, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireProduct& wire, MarketType market) {
    if (market != MarketType::Spot) {
        return std::nullopt;
    }
    if (wire.status != "online" || wire.trading_disabled) {
        return std::nullopt;
    }

    const auto tick = parse_decimal_e9(wire.quote_increment);
    const auto step = parse_decimal_e9(wire.base_increment);
    if (!tick.has_value() || !step.has_value() || tick->raw <= 0 || step->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.id);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Coinbase, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *step;
    if (const auto min_funds = parse_decimal_e9(wire.min_market_funds);
        min_funds.has_value() && min_funds->raw > 0) {
        base.min_notional = *min_funds;
    }
    base.contract_value = *parse_decimal_e9("1");
    return SpotInstrument{base};
}

} // namespace raikiri::venues::coinbase
