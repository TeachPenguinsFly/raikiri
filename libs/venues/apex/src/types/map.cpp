#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/apex/map.hpp>

#include <charconv>
#include <chrono>

namespace raikiri::venues::apex {

namespace {

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<int> digits(std::string_view text, std::size_t at, std::size_t len) noexcept {
    int out = 0;
    const auto* first = text.data() + at;
    const auto [stop, ec] = std::from_chars(first, first + len, out);
    return ec == std::errc{} && stop == first + len ? std::optional{out} : std::nullopt;
}

} // namespace

UnixNanos us_to_unix(std::uint64_t us, UnixNanos fallback) noexcept {
    return us == 0 ? fallback : UnixNanos{us * 1'000ULL};
}

std::optional<UnixNanos> parse_utc(std::string_view text) noexcept {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return std::nullopt;
    }
    const auto year = digits(text, 0, 4);
    const auto month = digits(text, 5, 2);
    const auto day = digits(text, 8, 2);
    const auto hour = digits(text, 11, 2);
    const auto minute = digits(text, 14, 2);
    const auto second = digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
        *second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch().count();
    const auto secs = static_cast<std::int64_t>(days) * 86'400 + *hour * 3'600 + *minute * 60 + *second;
    if (secs < 0) {
        return std::nullopt;
    }
    return UnixNanos{static_cast<std::uint64_t>(secs) * 1'000'000'000ULL};
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    if (market != MarketType::Perp || !wire.enable_trade) {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.step_size);
    const auto symbol = SymbolText::from(wire.cross_symbol);
    const auto local = canonical_symbol(wire.base, wire.settle);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 ||
        !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Apex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;

    PerpInstrument perp{base, k_funding_interval_s};
    if (const auto cap = parse_rate_e9(wire.funding_max_rate); cap.has_value()) {
        perp.funding_cap = *cap;
    }
    if (const auto floor = parse_rate_e9(wire.funding_min_rate); floor.has_value()) {
        perp.funding_floor = *floor;
    }
    return perp;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.size, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.buyer_aggressor ? Side::Buy : Side::Sell;
    out.ts_event = wire.ts_ms == 0 ? ts_init : UnixNanos{wire.ts_ms * 1'000'000ULL};
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireTicker& wire,
                                         const PerpInstrument& perp,
                                         FundingMemo& memo,
                                         UnixNanos ts_init) {
    const auto& base = perp.base;
    bool told = false;
    if (const auto rate = parse_rate_e9(wire.funding_rate); rate.has_value()) {
        memo.rate = *rate;
        told = true;
    }
    // the mark is quoted finer than the tick, so it is truncated onto the grid
    if (const auto mark = parse_decimal_e9(wire.mark_price);
        mark.has_value() && base.tick_size.raw > 0) {
        memo.mark = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
        told = true;
    }
    if (const auto next = parse_utc(wire.next_funding_time); next.has_value()) {
        memo.next_funding = *next;
        told = true;
    }
    if (!told) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    // fundingRate is what the hour pays; predictedFundingRate is a fixed interest base, not a forecast
    out.rate = memo.rate;
    out.mark_price = memo.mark;
    out.next_funding_time = memo.next_funding;
    out.interval_s = perp.funding_interval_s;
    out.ts_event = us_to_unix(wire.ts_us, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (wire.volume_24h.empty() && wire.turnover_24h.empty()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    if (const auto volume = parse_amount_e9(wire.volume_24h); volume.has_value()) {
        out.volume_24h_base = *volume;
    }
    if (const auto turnover = parse_turnover_e6(wire.turnover_24h); turnover.has_value()) {
        out.volume_24h_quote = *turnover;
    }
    out.ts_event = us_to_unix(wire.ts_us, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::apex
