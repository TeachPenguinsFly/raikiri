#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/arcus/map.hpp>

namespace raikiri::venues::arcus {

namespace {

constexpr std::uint64_t k_ns_per_us = 1'000ULL;
constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

UnixNanos us_to_unix(std::uint64_t us) noexcept {
    return UnixNanos{us * k_ns_per_us};
}

AssetClass asset_class_of(std::string_view category) noexcept {
    if (category == "CRYPTO") {
        return AssetClass::Crypto;
    }
    // the index and commodity rows are exchange-traded funds: SPY, QQQ, GLD, USO
    if (category == "EQUITIES" || category == "INDICES" || category == "COMMODITIES") {
        return AssetClass::Equity;
    }
    return AssetClass::Unknown;
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    if (market != MarketType::Perp || wire.type != "PERPETUAL" || wire.status != "ONLINE") {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick);
    const auto lot = parse_decimal_e9(wire.step);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(wire.base, wire.quote);
    const auto underlying = UnderlyingText::from(wire.base);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 || !symbol.has_value() ||
        !local.has_value() || !underlying.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Arcus, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min = parse_decimal_e9(wire.min_notional); min.has_value()) {
        base.min_notional = *min;
    }
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = asset_class_of(wire.category);
    base.underlying = *underlying;
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto bid = steps_of(wire.bid, base.tick_size);
    const auto bid_qty = steps_of(wire.bid_qty, base.lot_size);
    const auto ask = steps_of(wire.ask, base.tick_size);
    const auto ask_qty = steps_of(wire.ask_qty, base.lot_size);
    if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() || !ask_qty.has_value() || *bid_qty == 0 ||
        *ask_qty == 0 || *bid >= *ask) {
        return std::nullopt;
    }
    return Bbo{.instrument = base.id,
               .bid = PriceTicks::from_raw(*bid),
               .ask = PriceTicks::from_raw(*ask),
               .bid_qty = QtyLots::from_raw(*bid_qty),
               .ask_qty = QtyLots::from_raw(*ask_qty),
               .ts_event = us_to_unix(wire.ts_us),
               .ts_init = ts_init};
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    Side aggressor{};
    if (wire.side == "BUY") {
        aggressor = Side::Buy;
    } else if (wire.side == "SELL") {
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
    out.ts_event = us_to_unix(wire.ts_us);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireStats& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.funding);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto next = parse_rate_e9(wire.next_funding); next.has_value()) {
        out.predicted_rate = *next;
    }
    if (const auto mark = parse_amount_e9(wire.mark); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    if (const auto index = parse_amount_e9(wire.oracle); index.has_value() && base.tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(index->raw / base.tick_size.raw);
    }
    out.next_funding_time = UnixNanos{wire.next_funding_s * k_ns_per_s};
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireStats& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.volume_notional);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = *quote;
    if (const auto open = parse_amount_e9(wire.open_interest); open.has_value()) {
        out.open_interest = *open;
    }
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::arcus
