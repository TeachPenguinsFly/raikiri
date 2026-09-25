#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/pacifica/map.hpp>

namespace raikiri::venues::pacifica {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_interval_ns = std::uint64_t{k_funding_interval_s} * 1'000'000'000ULL;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * k_ns_per_ms};
}

UnixNanos next_settlement(UnixNanos at) noexcept {
    return UnixNanos{(at.ns / k_interval_ns + 1U) * k_interval_ns};
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    if (market != MarketType::Perp || wire.type != "perpetual") {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick);
    const auto lot = parse_decimal_e9(wire.lot);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(wire.symbol, k_perp_quote);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 || !symbol.has_value() ||
        !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Pacifica, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min = parse_decimal_e9(wire.min_notional); min.has_value()) {
        base.min_notional = *min;
    }
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
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
               .ts_event = ms_to_unix(wire.ts_ms, ts_init),
               .ts_init = ts_init};
}

std::optional<WireBbo> top_of(const WireBook& wire) noexcept {
    if (wire.bids.empty() || wire.asks.empty()) {
        return std::nullopt;
    }
    return WireBbo{.symbol = wire.symbol,
                   .li = wire.li,
                   .ts_ms = wire.ts_ms,
                   .bid = wire.bids.front().price,
                   .bid_qty = wire.bids.front().qty,
                   .ask = wire.asks.front().price,
                   .ask_qty = wire.asks.front().qty};
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    out.venue_seq = wire.li;
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

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    Side aggressor{};
    if (wire.direction == "open_long" || wire.direction == "close_short") {
        aggressor = Side::Buy;
    } else if (wire.direction == "open_short" || wire.direction == "close_long") {
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
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WirePrice& wire, const InstrumentBase& base, UnixNanos ts_init) {
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
    // the mark has more places than the tick, so it is truncated onto it
    if (const auto mark = parse_amount_e9(wire.mark); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.next_funding_time = next_settlement(out.ts_event);
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_init = ts_init;
    return out;
}

void FundingRoll::apply(FundingUpdate& update) noexcept {
    const auto at = update.ts_event.ns;
    const auto now_hour = at / k_interval_ns;
    if (hour != 0 && now_hour > hour) {
        hold_until_ns = now_hour * k_interval_ns + k_roll_hold_ns;
        held = funding;
    }
    if (hold_until_ns != 0 &&
        (at >= hold_until_ns || update.rate != held || (update.rate == next && update.predicted_rate != next))) {
        hold_until_ns = 0;
    }
    hour = now_hour;
    funding = update.rate;
    next = update.predicted_rate;
    if (hold_until_ns != 0) {
        update.rate = update.predicted_rate;
        update.predicted_rate = FixedE9::undef();
    }
}

std::optional<TickerStats> map_ticker_stats(const WirePrice& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.volume_24h);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = *quote;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::pacifica
