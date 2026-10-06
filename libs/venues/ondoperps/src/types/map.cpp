#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/ondoperps/map.hpp>

namespace raikiri::venues::ondoperps {

namespace {

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

AssetClass class_of(std::string_view tag) noexcept {
    if (tag == "Crypto") {
        return AssetClass::Crypto;
    }
    if (tag == "Stock" || tag == "ETF") {
        return AssetClass::Equity;
    }
    if (tag == "Index") {
        return AssetClass::Index;
    }
    if (tag == "Commodity") {
        return AssetClass::Commodity;
    }
    if (tag == "FX") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

std::optional<std::uint32_t>
fill_side(std::span<const WireLevel> src, std::array<BookLevel, k_book_levels>& dst, const InstrumentBase& base) {
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
}

std::optional<PriceTicks> truncated(std::string_view price, const InstrumentBase& base) noexcept {
    const auto value = parse_amount_e9(price);
    if (!value.has_value() || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(value->raw / base.tick_size.raw);
}

} // namespace

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    if (market != MarketType::Perp || wire.disabled) {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick);
    const auto lot = parse_decimal_e9(wire.lot);
    // margined and settled in usdc
    const auto quote = wire.quote == "USD" ? std::string_view{"USDC"} : wire.quote;
    const auto symbol = SymbolText::from(wire.market);
    const auto local = canonical_symbol(wire.base, quote);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 || !symbol.has_value() ||
        !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::OndoPerps, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = class_of(wire.tag);
    if (base.asset_class == AssetClass::FX) {
        if (const auto key = UnderlyingText::from(wire.base); key.has_value()) {
            base.underlying = *key;
        }
    }
    if (const auto fee = parse_rate_e9(wire.maker_fee); fee.has_value()) {
        base.maker_fee = *fee;
    }
    if (const auto fee = parse_rate_e9(wire.taker_fee); fee.has_value()) {
        base.taker_fee = *fee;
    }
    PerpInstrument out{base, k_funding_interval_s};
    if (const auto cap = parse_rate_e9(wire.funding_cap); cap.has_value()) {
        out.funding_cap = *cap;
        out.funding_floor = FixedE9::from_raw(-cap->raw);
    }
    return out;
}

std::optional<Bbo> map_top(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (wire.bids.empty() || wire.asks.empty()) {
        return std::nullopt;
    }
    const auto bid = steps_of(wire.bids.front().price, base.tick_size);
    const auto bid_qty = steps_of(wire.bids.front().qty, base.lot_size);
    const auto ask = steps_of(wire.asks.front().price, base.tick_size);
    const auto ask_qty = steps_of(wire.asks.front().qty, base.lot_size);
    if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }
    return Bbo{.instrument = base.id,
               .bid = PriceTicks::from_raw(*bid),
               .ask = PriceTicks::from_raw(*ask),
               .bid_qty = QtyLots::from_raw(*bid_qty),
               .ask_qty = QtyLots::from_raw(*ask_qty),
               .ts_event = UnixNanos{wire.ts_ns},
               .ts_init = ts_init};
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    const auto bids = fill_side(wire.bids, out.bids, base);
    const auto asks = fill_side(wire.asks, out.asks, base);
    if (!bids.has_value() || !asks.has_value() || (*bids == 0 && *asks == 0)) {
        return std::nullopt;
    }
    out.bid_count = *bids;
    out.ask_count = *asks;
    return out;
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    Side aggressor{};
    if (wire.side == "buy") {
        aggressor = Side::Buy;
    } else if (wire.side == "sell") {
        aggressor = Side::Sell;
    } else {
        return std::nullopt;
    }
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.size, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty == 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = aggressor;
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    return out;
}

std::optional<PriceTicks> mark_ticks(const WireMark& wire, const InstrumentBase& base) noexcept {
    return truncated(wire.mark, base);
}

std::optional<PriceTicks> oracle_ticks(const WireMark& wire, const InstrumentBase& base) noexcept {
    return truncated(wire.oracle, base);
}

std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, PriceTicks mark, PriceTicks index, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.mark_price = mark;
    out.index_price = index;
    out.next_funding_time = UnixNanos{wire.ends_ns};
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    return out;
}

TickerStats map_ticker(const Stats& stats, const InstrumentBase& base, UnixNanos ts_init) {
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = stats.volume_base;
    out.volume_24h_quote = stats.volume_quote;
    out.open_interest = stats.open_interest;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::ondoperps
