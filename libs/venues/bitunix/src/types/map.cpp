#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/bitunix/map.hpp>

#include <cctype>
#include <string>

namespace raikiri::venues::bitunix {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_max_places = 9;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

bool linear_quote(std::string_view quote) noexcept {
    return quote == "USDT" || quote == "USDC";
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * k_ns_per_ms};
}

std::optional<FixedE9> step_of(std::uint64_t places) noexcept {
    if (places > k_max_places) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::uint64_t i = 0; i < places; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

std::string precision_text(FixedE9 tick) {
    std::size_t places = 0;
    for (auto raw = tick.raw; raw > 0 && raw < k_e9 && places < k_max_places; raw *= 10) {
        ++places;
    }
    if (places == 0) {
        return "1";
    }
    std::string out{"0."};
    out.append(places - 1, '0');
    out.push_back('1');
    return out;
}

std::optional<FixedE9> percent_to_fraction(std::string_view percent) noexcept {
    const auto rate = parse_rate_e9(percent);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    return FixedE9::from_raw(rate->raw / 100);
}

std::optional<Instrument> map_pair(const WirePair& wire, MarketType market) {
    if (!wire.api || wire.status != "OPEN" || !linear_quote(wire.quote)) {
        return std::nullopt;
    }
    const auto tick = step_of(wire.quote_precision);
    const auto lot = step_of(wire.base_precision);
    std::string upper{wire.symbol};
    for (auto& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const auto symbol = SymbolText::from(upper);
    const auto local = canonical_symbol(wire.base, wire.quote);
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bitunix, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    PerpInstrument perp{};
    perp.base = base;
    if (const auto cap = percent_to_fraction(wire.max_funding); cap.has_value()) {
        perp.funding_cap = *cap;
    }
    if (const auto floor = percent_to_fraction(wire.min_funding); floor.has_value()) {
        perp.funding_floor = *floor;
    }
    return perp;
}

std::optional<Bbo> map_bbo(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (wire.bids.empty() || wire.asks.empty()) {
        return std::nullopt;
    }
    const auto bid = steps_of(wire.bids.front().price, base.tick_size);
    const auto bid_qty = steps_of(wire.bids.front().qty, base.lot_size);
    const auto ask = steps_of(wire.asks.front().price, base.tick_size);
    const auto ask_qty = steps_of(wire.asks.front().qty, base.lot_size);
    if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() || !ask_qty.has_value() || *bid_qty == 0 ||
        *ask_qty == 0) {
        return std::nullopt;
    }
    return Bbo{.instrument = base.id,
               .bid = PriceTicks::from_raw(*bid),
               .ask = PriceTicks::from_raw(*ask),
               .bid_qty = QtyLots::from_raw(*bid_qty),
               .ask_qty = QtyLots::from_raw(*ask_qty),
               .ts_event = ms_to_unix(wire.ts_ms, UnixNanos{}),
               .ts_init = ts_init};
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    using Levels = std::array<BookLevel, k_book_levels>;
    const auto fill = [&](std::span<const WireLevel> src, Levels& dst) -> std::optional<std::uint32_t> {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = steps_of(level.price, base.tick_size);
            const auto qty = steps_of(level.qty, base.lot_size);
            if (!price.has_value() || !qty.has_value()) {
                return std::nullopt;
            }
            dst[n] = {.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
            ++n;
        }
        return n;
    };
    const auto bids = fill(wire.bids, out.bids);
    const auto asks = fill(wire.asks, out.asks);
    if (!bids.has_value() || !asks.has_value() || (*bids == 0 && *asks == 0)) {
        return std::nullopt;
    }
    out.bid_count = *bids;
    out.ask_count = *asks;
    return out;
}

std::optional<TradeTick>
map_trade(const WirePrint& wire, std::uint64_t ts_ms, const InstrumentBase& base, UnixNanos ts_init) {
    Side aggressor{};
    if (wire.side == "buy") {
        aggressor = Side::Buy;
    } else if (wire.side == "sell") {
        aggressor = Side::Sell;
    } else {
        return std::nullopt;
    }
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty == 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = aggressor;
    out.ts_event = ms_to_unix(ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireFunding& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto rate = percent_to_fraction(wire.rate);
    if (!rate.has_value() || wire.interval_h == 0) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.predicted_rate = FixedE9::undef();
    if (const auto mark = parse_amount_e9(wire.mark); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    if (const auto index = parse_amount_e9(wire.index); index.has_value() && base.tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(index->raw / base.tick_size.raw);
    }
    out.next_funding_time = ms_to_unix(wire.next_ms, UnixNanos{});
    out.interval_s = static_cast<std::uint32_t>(wire.interval_h * 3600U);
    out.accrual = FundingAccrual::Discrete;
    out.ts_event = UnixNanos{};
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.quote_volume);
    const auto volume = parse_amount_e9(wire.base_volume);
    if (!quote.has_value() || !volume.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = *volume;
    out.volume_24h_quote = *quote;
    // the rows carry no time
    out.ts_event = UnixNanos{};
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::bitunix
