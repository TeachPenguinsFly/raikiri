#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/lbank/map.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace raikiri::venues::lbank {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;

UnixNanos ms_or(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * k_ns_per_ms};
}

UnixNanos s_or(std::uint64_t s, UnixNanos fallback) noexcept {
    return s == 0 ? fallback : UnixNanos{s * k_ns_per_s};
}

std::optional<FixedE9> places_step(std::string_view places) noexcept {
    const auto n = parse_decimal_e9(places);
    if (!n.has_value() || n->raw < 0 || n->raw % k_e9 != 0 || n->raw / k_e9 > 9) {
        return std::nullopt;
    }
    std::int64_t step = k_e9;
    for (std::int64_t i = 0; i < n->raw / k_e9; ++i) {
        step /= 10;
    }
    return FixedE9::from_raw(step);
}

} // namespace

std::optional<std::int64_t> ticks_of(std::string_view text, FixedE9 tick) noexcept {
    const auto value = parse_number_e9(text);
    return value.has_value() ? book::exact_steps(*value, tick) : std::nullopt;
}

std::optional<std::int64_t> lots_of(std::string_view text, FixedE9 lot) noexcept {
    const auto value = parse_amount_e9(text);
    if (!value.has_value() || lot.raw <= 0 || value->raw < 0) {
        return std::nullopt;
    }
    return (value->raw + lot.raw / 2) / lot.raw;
}

std::optional<Instrument> map_perp_instrument(const WirePerpInstrument& wire, std::uint32_t funding_interval_s) {
    if (wire.suspended) {
        return std::nullopt;
    }
    const auto tick = parse_number_e9(wire.price_tick);
    const auto lot = parse_number_e9(wire.volume_tick);
    const auto multiple = parse_number_e9(wire.multiple);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(wire.base, wire.quote);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 || !symbol.has_value() ||
        !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Lbank, MarketType::Perp, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min = parse_number_e9(wire.min_cost); min.has_value()) {
        base.min_notional = *min;
    }
    base.contract_value = multiple.has_value() && multiple->raw > 0 ? *multiple : FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, funding_interval_s};
}

std::optional<Instrument> map_spot_instrument(const WireSpotPair& wire) {
    const auto cut = wire.symbol.find('_');
    const auto tick = places_step(wire.price_places);
    const auto lot = places_step(wire.qty_places);
    const auto symbol = SymbolText::from(wire.symbol);
    if (cut == std::string_view::npos || !tick.has_value() || !lot.has_value() || !symbol.has_value()) {
        return std::nullopt;
    }
    std::array<char, 32> base_up{};
    std::array<char, 32> quote_up{};
    const auto upper = [](std::string_view in, std::span<char> out) -> std::string_view {
        if (in.empty() || in.size() > out.size()) {
            return {};
        }
        for (std::size_t i = 0; i < in.size(); ++i) {
            const char c = in[i];
            out[i] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }
        return std::string_view{out.data(), in.size()};
    };
    const auto local = canonical_symbol(upper(wire.symbol.substr(0, cut), base_up),
                                        upper(wire.symbol.substr(cut + 1), quote_up));
    if (!local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Lbank, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min = parse_number_e9(wire.min_amount); min.has_value()) {
        base.min_notional = *min;
    }
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

bool apply_level(const WireLevel& level, const InstrumentBase& base, book::Ladders& ladders) {
    const auto price = ticks_of(level.price, base.tick_size);
    const auto qty = lots_of(level.volume, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return false;
    }
    auto& side = level.ask ? ladders.asks() : ladders.bids();
    if (*qty == 0) {
        side.remove(PriceTicks::from_raw(*price));
        return true;
    }
    side.upsert({.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)});
    // a resting order at this price proves the other side holds nothing through it
    auto& other = level.ask ? ladders.bids() : ladders.asks();
    while (!other.empty()) {
        const auto best = other.levels().front().price;
        if (level.ask ? best.raw < *price : best.raw > *price) {
            break;
        }
        other.remove(best);
    }
    return true;
}

bool apply_levels(const WireBook& wire, std::string_view symbol, const InstrumentBase& base, book::Ladders& ladders) {
    for (const auto& level : wire.levels) {
        if (level.instrument == symbol && level.tick.empty() && !apply_level(level, base, ladders)) {
            return false;
        }
    }
    return true;
}

bool seed_levels(const WireRestDepth& wire, const InstrumentBase& base, book::Ladders& ladders) {
    ladders.clear();
    const auto seed = [&](std::span<const WireDepthRow> rows, bool ask) {
        for (const auto& row : rows) {
            if (!apply_level({.price = row.price, .volume = row.volume, .ask = ask}, base, ladders)) {
                return false;
            }
        }
        return true;
    };
    return seed(wire.bids, false) && seed(wire.asks, true);
}

bool same_touch(const book::Ladders& ladders, const WireRestDepth& wire, const InstrumentBase& base) {
    const auto touch = ladders.touch();
    if (!touch.has_value() || wire.bids.empty() || wire.asks.empty()) {
        return false;
    }
    return ticks_of(wire.bids.front().price, base.tick_size) == touch->first.price.raw &&
           ticks_of(wire.asks.front().price, base.tick_size) == touch->second.price.raw;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = ticks_of(wire.price, base.tick_size);
    const auto qty = lots_of(wire.volume, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty == 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.buy ? Side::Buy : Side::Sell;
    out.ts_event = s_or(wire.time_s, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireOverview& wire, const InstrumentBase& base, std::uint32_t interval_s, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.fee_rate);
    if (wire.fee_rate.empty() || !rate.has_value() || interval_s == 0) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.predicted_rate = FixedE9::undef();
    if (const auto mark = parse_amount_e9(wire.mark); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    out.ts_event = s_or(wire.update_s, UnixNanos{});
    const auto interval_ns = std::uint64_t{interval_s} * k_ns_per_s;
    out.next_funding_time = UnixNanos{(out.ts_event.ns / interval_ns + 1U) * interval_ns};
    out.interval_s = interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_perp_stats(const WireOverview& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.turnover_24h);
    if (wire.turnover_24h.empty() || !quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    if (const auto volume = parse_amount_e9(wire.volume_24h); volume.has_value()) {
        out.volume_24h_base = *volume;
    }
    out.volume_24h_quote = *quote;
    out.ts_event = s_or(wire.update_s, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> map_spot_book(const WireSpotDepth& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_or(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    out.venue_seq = wire.ts_ms;
    const auto fill = [&](std::span<const WirePair> src,
                          std::array<BookLevel, k_book_levels>& dst) -> std::optional<std::uint32_t> {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = ticks_of(level.price, base.tick_size);
            const auto qty = lots_of(level.qty, base.lot_size);
            if (!price.has_value() || !qty.has_value()) {
                return std::nullopt;
            }
            if (*qty == 0) {
                continue;
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

std::optional<Bbo> top_of(const BookSnapshot& book) noexcept {
    if (book.bid_count == 0 || book.ask_count == 0) {
        return std::nullopt;
    }
    return Bbo{.instrument = book.instrument,
               .bid = book.bids[0].price,
               .ask = book.asks[0].price,
               .bid_qty = book.bids[0].qty,
               .ask_qty = book.asks[0].qty,
               .ts_event = book.ts_event,
               .ts_init = book.ts_init};
}

std::optional<TradeTick> map_spot_trade(const WireSpotTrade& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = ticks_of(wire.price, base.tick_size);
    const auto qty = lots_of(wire.volume, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty == 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.buy ? Side::Buy : Side::Sell;
    out.ts_event = ms_or(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_spot_stats(const WireSpotTick& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.turnover);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    if (const auto volume = parse_amount_e9(wire.volume); volume.has_value()) {
        out.volume_24h_base = *volume;
    }
    out.volume_24h_quote = *quote;
    out.ts_event = ms_or(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::lbank
