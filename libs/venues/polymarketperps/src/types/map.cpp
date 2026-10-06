#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/polymarketperps/map.hpp>

namespace raikiri::venues::polymarketperps {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<FixedE9> decimal_step(std::uint32_t decimals) noexcept {
    if (decimals > 9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::uint32_t i = 0; i < decimals; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

AssetClass class_of(std::string_view category) noexcept {
    if (category == "crypto") {
        return AssetClass::Crypto;
    }
    if (category == "equity") {
        return AssetClass::Equity;
    }
    if (category == "index") {
        return AssetClass::Index;
    }
    if (category == "commodity") {
        return AssetClass::Commodity;
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

} // namespace

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (market != MarketType::Perp || wire.type != "perpetual" || wire.close_only) {
        return std::nullopt;
    }
    // the price rule keeps five significant digits; price_decimals is its finest step
    const auto tick = decimal_step(wire.price_decimals);
    const auto lot = decimal_step(wire.quantity_decimals);
    const auto symbol = SymbolText::from(wire.symbol);
    // pusd is the venue's usdc
    const auto local = canonical_symbol(wire.base, "USDC");
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::PolymarketPerps, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = class_of(wire.category);
    if (const auto min = parse_decimal_e9(wire.min_notional); min.has_value()) {
        base.min_notional = *min;
    }
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto bid = steps_of(wire.bid, base.tick_size);
    const auto bid_qty = steps_of(wire.bid_qty, base.lot_size);
    const auto ask = steps_of(wire.ask, base.tick_size);
    const auto ask_qty = steps_of(wire.ask_qty, base.lot_size);
    if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() || !ask_qty.has_value() || *bid_qty == 0 ||
        *ask_qty == 0) {
        return std::nullopt;
    }
    return Bbo{.instrument = base.id,
               .bid = PriceTicks::from_raw(*bid),
               .ask = PriceTicks::from_raw(*ask),
               .bid_qty = QtyLots::from_raw(*bid_qty),
               .ask_qty = QtyLots::from_raw(*ask_qty),
               .ts_event = UnixNanos{wire.ts_ms * k_ns_per_ms},
               .ts_init = ts_init};
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = UnixNanos{wire.ts_ms * k_ns_per_ms};
    out.ts_init = ts_init;
    out.venue_seq = wire.sq;
    const auto bids = fill_side(wire.bids, out.bids, base);
    const auto asks = fill_side(wire.asks, out.asks, base);
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

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (wire.settlement) {
        return std::nullopt;
    }
    Side aggressor{};
    if (wire.side == "long") {
        aggressor = Side::Buy;
    } else if (wire.side == "short") {
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
    out.ts_event = UnixNanos{wire.ts_ms * k_ns_per_ms};
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.funding);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = parse_amount_e9(wire.mark); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    if (const auto index = parse_amount_e9(wire.index); index.has_value() && base.tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(index->raw / base.tick_size.raw);
    }
    out.next_funding_time = UnixNanos{wire.next_funding_ms * k_ns_per_ms};
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_event = UnixNanos{wire.ts_ms * k_ns_per_ms};
    out.ts_init = ts_init;
    return out;
}

TickerStats map_ticker(const Stats& stats, const InstrumentBase& base, UnixNanos ts_init) {
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = stats.volume_quote;
    out.open_interest = stats.open_interest;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::polymarketperps
