#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/dydx/map.hpp>

#include <limits>

namespace raikiri::venues::dydx {

namespace {

constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;

// USDC's atomic resolution, fixed by the chain.
constexpr std::int64_t k_quote_resolution = -6;

std::optional<FixedE9> scaled(std::uint64_t mantissa, std::int64_t exp10) noexcept {
    if (mantissa == 0 || exp10 < -18 || exp10 > 18) {
        return std::nullopt;
    }
    int128_t value = mantissa;
    if (exp10 >= 0) {
        for (std::int64_t i = 0; i < exp10; ++i) {
            value *= 10;
        }
    } else {
        int128_t divisor = 1;
        for (std::int64_t i = 0; i < -exp10; ++i) {
            divisor *= 10;
        }
        if (value % divisor != 0) {
            return std::nullopt;
        }
        value /= divisor;
    }
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::nullopt;
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(value));
}

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

constexpr std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
    y -= static_cast<std::int64_t>(m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146'097 + doe - 719'468;
}

} // namespace

std::string_view to_string(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::Inactive:
        return "inactive";
    case Refusal::TooFine:
        return "grid finer than 1e-9";
    case Refusal::Disagrees:
        return "tickSize/stepSize disagree with the quantum fields";
    case Refusal::Unreadable:
        return "unreadable";
    }
    return "unknown";
}

std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) noexcept {
    if (text.empty() || step.raw <= 0 || text.size() > 30) {
        return std::nullopt;
    }
    int128_t value = 0;
    std::size_t fraction = 0;
    bool point = false;
    for (const char c : text) {
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        if (c < '0' || c > '9' || (point && ++fraction > 9)) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    for (; fraction < 9; ++fraction) {
        value *= 10;
    }
    if (value % step.raw != 0 || value / step.raw > std::numeric_limits<std::int64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value / step.raw);
}

std::optional<FixedE9> quantum_tick(const WireMarket& wire) noexcept {
    return scaled(wire.subticks_per_tick,
                  wire.quantum_conversion_exponent - wire.atomic_resolution + k_quote_resolution +
                      9);
}

std::optional<FixedE9> quantum_lot(const WireMarket& wire) noexcept {
    return scaled(wire.step_base_quantums, wire.atomic_resolution + 9);
}

std::expected<Instrument, Refusal> map_instrument(const WireMarket& wire) {
    if (wire.status != "ACTIVE") {
        return std::unexpected(Refusal::Inactive);
    }
    const auto tick = quantum_tick(wire);
    const auto lot = quantum_lot(wire);
    if (!tick.has_value() || !lot.has_value()) {
        return std::unexpected(Refusal::TooFine);
    }
    const auto stated_tick = parse_decimal_e9(wire.tick_size);
    const auto stated_lot = parse_decimal_e9(wire.step_size);
    if (!stated_tick.has_value() || !stated_lot.has_value() || *stated_tick != *tick ||
        *stated_lot != *lot) {
        return std::unexpected(Refusal::Disagrees);
    }
    if (!wire.ticker.ends_with(k_ticker_suffix)) {
        return std::unexpected(Refusal::Unreadable);
    }
    const auto asset = wire.ticker.substr(0, wire.ticker.size() - k_ticker_suffix.size());
    const auto symbol = SymbolText::from(wire.ticker);
    const auto local = canonical_symbol(asset, k_margin_asset);
    if (asset.empty() || !symbol.has_value() || !local.has_value()) {
        return std::unexpected(Refusal::Unreadable);
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Dydx, MarketType::Perp, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = exact_steps(wire.price, base.tick_size);
    const auto qty = exact_steps(wire.size, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    Side aggressor{};
    if (wire.side == "BUY") {
        aggressor = Side::Buy;
    } else if (wire.side == "SELL") {
        aggressor = Side::Sell;
    } else {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = aggressor;
    out.ts_event = parse_iso8601(wire.created_at).value_or(ts_init);
    out.ts_init = ts_init;
    return out;
}

// parse_rate_e9 only drops what lies below 1e-9, which is below every tick
// loaded, so the truncation onto the tick is the only rounding there is.
std::optional<PriceTicks> oracle_ticks(std::string_view text, FixedE9 tick) noexcept {
    const auto value = parse_rate_e9(text);
    if (!value.has_value() || value->raw <= 0 || tick.raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(value->raw / tick.raw);
}

UnixNanos next_hour(UnixNanos now) noexcept {
    constexpr std::uint64_t interval = std::uint64_t{k_funding_interval_s} * k_ns_per_s;
    return UnixNanos{(now.ns / interval + 1) * interval};
}

std::optional<FundingUpdate>
map_funding(const WireMarket& wire, const InstrumentBase& base, UnixNanos ts_init) {
    // parse_rate_e9, not the exact reader: the rate is a mean of hourly samples
    // and runs to 20 decimals ("0.00002770535714285714").
    const auto rate =
        wire.next_funding_rate.empty() ? std::nullopt : parse_rate_e9(wire.next_funding_rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    // nextFundingRate is the hour now accruing, paid at next_funding_time — what
    // `rate` means on every venue here. It omits default funding and the clamp the
    // settlement adds, and nothing forecasts the hour after, so predicted_rate stays UNDEF.
    out.rate = *rate;
    if (const auto mark = oracle_ticks(wire.oracle_price, base.tick_size); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = next_hour(ts_init);
    out.interval_s = k_funding_interval_s;
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireMarket& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto turnover = parse_turnover_e6(wire.volume_24h);
    if (!turnover.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = *turnover;
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<UnixNanos> parse_iso8601(std::string_view text) noexcept {
    constexpr std::size_t head = 19; // YYYY-MM-DDTHH:MM:SS
    if (text.size() < head + 1 || text.back() != 'Z' || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto year = digits_at(text, 0, 4);
    const auto month = digits_at(text, 5, 2);
    const auto day = digits_at(text, 8, 2);
    const auto hour = digits_at(text, 11, 2);
    const auto minute = digits_at(text, 14, 2);
    const auto second = digits_at(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *year < 1970 || *month < 1 ||
        *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    std::uint64_t frac_ns = 0;
    std::size_t at = head;
    if (text[at] == '.') {
        std::uint64_t scale = 100'000'000;
        for (++at; at + 1 < text.size(); ++at) {
            const char c = text[at];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            frac_ns += static_cast<std::uint64_t>(c - '0') * scale;
            scale /= 10;
        }
    }
    if (at + 1 != text.size()) {
        return std::nullopt;
    }
    const auto days = days_from_civil(*year, *month, *day);
    const auto seconds = days * 86'400 + *hour * 3'600 + *minute * 60 + *second;
    return UnixNanos{static_cast<std::uint64_t>(seconds) * k_ns_per_s + frac_ns};
}

} // namespace raikiri::venues::dydx
