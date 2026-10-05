#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/cryptocom/map.hpp>

#include <optional>

namespace raikiri::venues::cryptocom {

namespace {

constexpr std::uint64_t k_interval_ms = std::uint64_t{k_funding_interval_s} * 1000;

// off-step is refused, not rounded
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

FixedE9 amount_or_undef(std::string_view text) {
    if (text.empty()) {
        return FixedE9::undef();
    }
    return parse_amount_e9(text).value_or(FixedE9::undef());
}

FundingUpdate merged(const InstrumentBase& base, const FundingMemo& memo, UnixNanos ts_event, UnixNanos ts_init) {
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = memo.rate;
    out.predicted_rate = memo.predicted;
    out.mark_price = memo.mark;
    out.next_funding_time = ms_to_unix(next_settlement_ms(memo.rate_ms), UnixNanos{0});
    out.interval_s = k_funding_interval_s;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
publish_if_rated(const InstrumentBase& base, const FundingMemo& memo, std::uint64_t ms, UnixNanos ts_init) {
    if (memo.rate.is_undef()) {
        return std::nullopt;
    }
    return merged(base, memo, ms_to_unix(ms, UnixNanos{}), ts_init);
}

} // namespace

std::uint64_t next_settlement_ms(std::uint64_t ms) noexcept {
    if (ms == 0) {
        return 0;
    }
    return (ms / k_interval_ms + 1) * k_interval_ms;
}

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = exact_steps(wire.price, base.tick_size);
    const auto qty = exact_steps(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    if (wire.side == "BUY") {
        out.aggressor = Side::Buy;
    } else if (wire.side == "SELL") {
        out.aggressor = Side::Sell;
    } else {
        return std::nullopt;
    }
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.ts_event = ms_to_unix(wire.trade_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = amount_or_undef(wire.volume);
    out.volume_24h_quote = parse_turnover_e6(wire.quote_volume).value_or(FixedE6::undef());
    if (out.volume_24h_base.is_undef() && out.volume_24h_quote.is_undef()) {
        return std::nullopt;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding_rate(const WireValue& wire, const InstrumentBase& base, FundingMemo& memo, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.value);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    memo.rate = *rate;
    memo.rate_ms = wire.ts_ms;
    return publish_if_rated(base, memo, wire.ts_ms, ts_init);
}

std::optional<FundingUpdate>
map_estimated_rate(const WireValue& wire, const InstrumentBase& base, FundingMemo& memo, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.value);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    memo.predicted = *rate;
    return publish_if_rated(base, memo, wire.ts_ms, ts_init);
}

std::optional<FundingUpdate>
map_mark(const WireValue& wire, const InstrumentBase& base, FundingMemo& memo, UnixNanos ts_init) {
    const auto mark = exact_steps(wire.value, base.tick_size);
    if (!mark.has_value()) {
        return std::nullopt;
    }
    memo.mark = PriceTicks::from_raw(*mark);
    return publish_if_rated(base, memo, wire.ts_ms, ts_init);
}

namespace {

AssetClass asset_class_of(std::string_view product_type) noexcept {
    if (product_type == "DIGITAL_CURRENCIES") {
        return AssetClass::Crypto;
    }
    if (product_type == "EQUITY" || product_type == "PRE_IPO") {
        return AssetClass::Equity;
    }
    if (product_type == "EQUITY_IND") {
        return AssetClass::Index;
    }
    if (product_type == "COMMODITIES") {
        return AssetClass::Commodity;
    }
    if (product_type == "CURRENCIES") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

} // namespace

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    const bool spot = wire.inst_type == "CCY_PAIR";
    const bool perp = wire.inst_type == "PERPETUAL_SWAP";
    if ((market == MarketType::Spot && !spot) || (market == MarketType::Perp && !perp) ||
        (market != MarketType::Spot && market != MarketType::Perp)) {
        return std::nullopt;
    }
    if (!wire.tradable) {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.price_tick_size);
    const auto lot = parse_decimal_e9(wire.qty_tick_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }
    // perpetuals quote the venue's usd bundle ("BTC/USD"), not folded into usdt
    const auto canonical = canonical_symbol(wire.base_ccy, wire.quote_ccy);
    if (!canonical.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::CryptoCom, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.kind = ContractKind::Linear;
    base.asset_class = asset_class_of(wire.product_type);
    if (spot) {
        base.contract_value = FixedE9::from_raw(k_e9);
        return SpotInstrument{base};
    }
    const auto contract = parse_decimal_e9(wire.contract_size);
    if (!contract.has_value() || contract->raw <= 0) {
        return std::nullopt;
    }
    base.contract_value = *contract;
    return PerpInstrument{base, k_funding_interval_s};
}

} // namespace raikiri::venues::cryptocom
