#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/paradex/map.hpp>

#include <limits>

namespace raikiri::venues::paradex {

namespace {

constexpr std::uint64_t k_ns_per_us = 1'000ULL;
constexpr std::uint32_t k_s_per_hour = 3600;
constexpr std::int64_t k_e8_to_e9 = 10;

std::optional<std::int64_t> steps_of(std::int64_t mantissa, FixedE9 step) noexcept {
    const auto value = e8_to_e9(mantissa);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::optional<FixedE9> e8_to_e9(std::int64_t mantissa) noexcept {
    constexpr auto k_max = std::numeric_limits<std::int64_t>::max() / k_e8_to_e9;
    if (mantissa == k_null || mantissa > k_max || mantissa < -k_max) {
        return std::nullopt;
    }
    return FixedE9::from_raw(mantissa * k_e8_to_e9);
}

UnixNanos us_to_unix(std::int64_t us) noexcept {
    return us > 0 ? UnixNanos{static_cast<std::uint64_t>(us) * k_ns_per_us} : UnixNanos{};
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    if (market != MarketType::Perp || wire.kind != "PERP") {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick);
    const auto lot = parse_decimal_e9(wire.lot);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(wire.base, wire.quote);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 || !symbol.has_value() ||
        !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Paradex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min = parse_decimal_e9(wire.min_notional); min.has_value()) {
        base.min_notional = *min;
    }
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    // the venue tags stocks, indices and commodities alike as rwa; the curated rows split the last two out
    base.asset_class = wire.rwa ? AssetClass::Equity : AssetClass::Crypto;

    PerpInstrument out{base, 0};
    if (const auto cap = parse_rate_e9(wire.max_funding_rate); cap.has_value() && cap->raw > 0) {
        out.funding_cap = *cap;
        out.funding_floor = FixedE9::from_raw(-cap->raw);
    }
    return out;
}

std::optional<book::Level> map_level(const WireLevel& wire, const InstrumentBase& base) noexcept {
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty < 0) {
        return std::nullopt;
    }
    return book::Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    Side aggressor{};
    if (wire.side == 1) {
        aggressor = Side::Buy;
    } else if (wire.side == 2) {
        aggressor = Side::Sell;
    } else {
        return std::nullopt;
    }
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty <= 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = aggressor;
    out.ts_event = us_to_unix(wire.created_us);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(
    const WireFunding& wire, const InstrumentBase& base, std::int64_t mark, std::int64_t index, UnixNanos ts_init) {
    const auto rate = e8_to_e9(wire.rate);
    if (!rate.has_value() || wire.period_hours <= 0) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.predicted_rate = FixedE9::undef();
    if (const auto price = e8_to_e9(mark); price.has_value() && price->raw > 0 && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(price->raw / base.tick_size.raw);
    }
    if (const auto price = e8_to_e9(index); price.has_value() && price->raw > 0 && base.tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(price->raw / base.tick_size.raw);
    }
    out.interval_s = static_cast<std::uint32_t>(wire.period_hours) * k_s_per_hour;
    out.accrual = FundingAccrual::Continuous;
    out.ts_event = us_to_unix(wire.created_us);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireSummary& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (wire.volume_usd == k_null || wire.volume_usd < 0) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = FixedE6::from_raw(wire.volume_usd / 100);
    if (const auto open = e8_to_e9(wire.open_interest); open.has_value() && open->raw >= 0) {
        out.open_interest = *open;
    }
    out.ts_event = us_to_unix(wire.ts_us);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::paradex
