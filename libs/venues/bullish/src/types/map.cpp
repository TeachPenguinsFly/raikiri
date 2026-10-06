#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/bullish/map.hpp>

namespace raikiri::venues::bullish {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_interval_ns = std::uint64_t{k_funding_interval_s} * 1'000'000'000ULL;
constexpr std::uint32_t k_max_places = 9;
constexpr std::int64_t k_percent = 100;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<FixedE9> step_of(std::uint32_t places) noexcept {
    if (places > k_max_places) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::uint32_t i = 0; i < places; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms) noexcept {
    return UnixNanos{ms * k_ns_per_ms};
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    const bool spot = market == MarketType::Spot && wire.type == "SPOT";
    const bool perp = market == MarketType::Perp && wire.type == "PERPETUAL";
    if (!wire.enabled || (!spot && !perp)) {
        return std::nullopt;
    }
    const auto order_tick = parse_decimal_e9(wire.tick);
    const auto tick = step_of(wire.price_precision);
    const auto lot = step_of(wire.quantity_precision);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(wire.base, wire.quote);
    if (!order_tick.has_value() || !tick.has_value() || !lot.has_value() || order_tick->raw <= 0 ||
        !book::exact_steps(*order_tick, *tick).has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bullish, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    if (spot) {
        return SpotInstrument{base};
    }
    if (const auto multiplier = parse_decimal_e9(wire.multiplier); multiplier.has_value() && multiplier->raw > 0) {
        base.contract_value = *multiplier;
    }
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms);
    out.ts_init = ts_init;
    out.venue_seq = wire.seq_last;
    const auto fill = [&](std::span<const WireLevel> src,
                          std::array<BookLevel, k_book_levels>& dst) -> std::optional<std::uint32_t> {
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

std::optional<Bbo> top_of(const BookSnapshot& book) noexcept {
    if (book.bid_count == 0 || book.ask_count == 0 || book.bids[0].price.raw >= book.asks[0].price.raw) {
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
    out.ts_event = ms_to_unix(wire.ts_ms);
    out.ts_init = ts_init;
    return out;
}

std::optional<PriceTicks> index_ticks(const WireIndex& wire, const InstrumentBase& base) noexcept {
    const auto price = parse_amount_e9(wire.price);
    if (!price.has_value() || price->raw <= 0 || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(price->raw / base.tick_size.raw);
}

std::optional<FundingUpdate>
map_funding(const WireTick& wire, const InstrumentBase& base, PriceTicks index, UnixNanos ts_init) {
    const auto percent = parse_rate_e9(wire.funding_rate);
    if (!percent.has_value() || wire.ts_ms == 0) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = FixedE9::from_raw(percent->raw / k_percent);
    out.predicted_rate = FixedE9::undef();
    if (const auto mark = parse_amount_e9(wire.mark); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    out.index_price = index;
    out.ts_event = ms_to_unix(wire.ts_ms);
    out.next_funding_time = UnixNanos{(out.ts_event.ns / k_interval_ns + 1U) * k_interval_ns};
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTick& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.quote_volume);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = *quote;
    if (const auto open = parse_amount_e9(wire.open_interest); open.has_value()) {
        out.open_interest = *open;
    }
    out.ts_event = ms_to_unix(wire.ts_ms);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::bullish
