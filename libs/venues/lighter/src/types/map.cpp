#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/lighter/map.hpp>

#include <limits>
#include <optional>

namespace raikiri::venues::lighter {

namespace {

// a usd amount at the mark; undefined without a positive mark or past i64
FixedE9 base_units(std::string_view usd_text, std::string_view mark_text) {
    const auto usd = parse_amount_e9(usd_text);
    const auto mark = parse_amount_e9(mark_text);
    if (!usd.has_value() || !mark.has_value() || mark->raw <= 0) {
        return FixedE9::undef();
    }
    const int128_t base = static_cast<int128_t>(usd->raw) * k_e9 / mark->raw;
    if (base > std::numeric_limits<std::int64_t>::max() || base < 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(base));
}

constexpr std::uint64_t k_interval_ms = std::uint64_t{k_funding_interval_s} * 1000;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

// an index is not a traded price and need not sit on the grid, so it is truncated to it
std::optional<PriceTicks> index_ticks(std::string_view text, FixedE9 tick) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || tick.raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(value->raw / tick.raw);
}

// Percent in, ratio out; exact or refused.
std::optional<FixedE9> percent(std::string_view text, std::int64_t divisor) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw % divisor != 0) {
        return std::nullopt;
    }
    return FixedE9::from_raw(value->raw / divisor);
}

std::optional<SymbolText> local_symbol(const WireMarket& wire, MarketType market) {
    if (market == MarketType::Perp) {
        return canonical_symbol(wire.symbol, k_perp_quote);
    }
    const auto halves = split_symbol(wire.symbol, '/');
    if (!halves.has_value()) {
        return std::nullopt;
    }
    return canonical_symbol(halves->first.view(), halves->second.view());
}

// lighter names no class, but it groups perps: 3 commodities, 4 currencies, 5 and 6 stocks and funds. a coin
// pays the full funding premium, even the one filed among the stocks
AssetClass asset_class_of(const WireMarket& wire, MarketType market) noexcept {
    if (market == MarketType::Spot || wire.funding_premium_multiplier >= 100) {
        return AssetClass::Crypto;
    }
    switch (wire.strategy_index) {
    case 3:
        return AssetClass::Commodity;
    case 4:
        return AssetClass::FX;
    case 5:
    case 6:
        return AssetClass::Equity;
    default:
        return AssetClass::Unknown;
    }
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_of(std::uint32_t decimals) noexcept {
    if (decimals > 9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::uint32_t i = 0; i < decimals; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    std::string_view wanted;
    switch (market) {
    case MarketType::Perp:
        wanted = "perp";
        break;
    case MarketType::Spot:
        wanted = "spot";
        break;
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    }
    if (wire.market_type != wanted || wire.status != "active") {
        return std::nullopt;
    }
    const auto tick = step_of(wire.price_decimals);
    const auto lot = step_of(wire.size_decimals);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = local_symbol(wire, market);
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Lighter, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto floor = parse_decimal_e9(wire.min_quote_amount); floor.has_value()) {
        base.min_notional = *floor;
    }
    // A spot row's `multiplier` is its token's, not what one unit of size buys.
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = asset_class_of(wire, market);
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }

    PerpInstrument perp{base, k_funding_interval_s};
    // funding_clamp_big is percent per eight hours, so the hourly cap is clamp / 8 / 100.
    if (const auto cap = percent(wire.funding_clamp_big, 800); cap.has_value()) {
        perp.funding_cap = *cap;
        perp.funding_floor = FixedE9::from_raw(-cap->raw);
    }
    return perp;
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.size, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.maker_is_ask ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireMarketStats& wire, const InstrumentBase& base, std::uint64_t frame_ms, UnixNanos ts_init) {
    if (wire.current_funding_rate.empty()) {
        return std::nullopt;
    }
    const auto rate = percent(wire.current_funding_rate, 100);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    // funding_rate is the hour already paid, so predicted_rate stays undef
    if (const auto mark = steps_of(wire.mark_price, base.tick_size); mark.has_value()) {
        out.mark_price = PriceTicks::from_raw(*mark);
    }
    if (const auto index = index_ticks(wire.index_price, base.tick_size); index.has_value()) {
        out.index_price = *index;
    }
    // A market idle past the hour gets no next time rather than one in the past.
    if (wire.funding_ms != 0) {
        const auto next = (wire.funding_ms / k_interval_ms + 1) * k_interval_ms;
        if (next > frame_ms) {
            out.next_funding_time = ms_to_unix(next, UnixNanos{0});
        }
    }
    out.interval_s = k_funding_interval_s;
    out.ts_event = ms_to_unix(frame_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireMarketStats& wire, const InstrumentBase& base, std::uint64_t frame_ms, UnixNanos ts_init) {
    // Go-encoded floats (89.40990000000001): an exact read would refuse them.
    const auto volume = parse_amount_e9(wire.base_volume);
    const auto turnover = parse_turnover_e6(wire.quote_volume);
    if (!volume.has_value() && !turnover.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    if (volume.has_value()) {
        out.volume_24h_base = *volume;
    }
    if (turnover.has_value()) {
        out.volume_24h_quote = *turnover;
    }
    out.open_interest = base_units(wire.open_interest, wire.mark_price);
    out.ts_event = ms_to_unix(frame_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::lighter
