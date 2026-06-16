#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/extended/map.hpp>

namespace raikiri::venues::extended {

namespace {

constexpr std::string_view k_wire_quote = "-USD";

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

// The mark carries more places than the tick, so it is truncated onto it.
std::optional<PriceTicks> mark_ticks(std::string_view text, const InstrumentBase& base) {
    const auto value = parse_amount_e9(text);
    if (!value.has_value() || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(value->raw / base.tick_size.raw);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    if (market != MarketType::Perp || wire.type != "PERPETUAL" || wire.status != "ACTIVE" ||
        !wire.name.ends_with(k_wire_quote)) {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick);
    const auto lot = parse_decimal_e9(wire.lot);
    const auto symbol = SymbolText::from(wire.name);
    // kFLOKI and 1000PEPE are a thousand of the coin and keep their own name.
    const auto local =
        canonical_symbol(wire.name.substr(0, wire.name.size() - k_wire_quote.size()), k_perp_quote);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 ||
        !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Extended, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;

    PerpInstrument perp{base, k_funding_interval_s};
    // hourlyFundingRateCap is not read: its unit differs from row to row.
    return perp;
}

std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.taker_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

bool note_rate(const WireFunding& wire, FundingMemo& memo) noexcept {
    const auto rate = parse_rate_e9(wire.rate);
    if (!rate.has_value()) {
        return false;
    }
    memo.rate = *rate;
    return true;
}

bool note_mark(const WireMark& wire, const InstrumentBase& base, FundingMemo& memo) noexcept {
    const auto mark = mark_ticks(wire.price, base);
    if (!mark.has_value()) {
        return false;
    }
    memo.mark = *mark;
    return true;
}

void note_market(const WireMarket& wire, const InstrumentBase& base, FundingMemo& memo) noexcept {
    if (memo.rate.is_undef()) {
        if (const auto rate = parse_rate_e9(wire.funding_rate); rate.has_value()) {
            memo.rate = *rate;
        }
    }
    if (memo.mark.is_undef()) {
        if (const auto mark = mark_ticks(wire.mark_price, base); mark.has_value()) {
            memo.mark = *mark;
        }
    }
    if (wire.next_funding_ms != 0) {
        memo.next_funding = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    }
}

FundingUpdate
merged(const InstrumentBase& base, const FundingMemo& memo, UnixNanos ts_event, UnixNanos ts_init) {
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = memo.rate;
    out.mark_price = memo.mark;
    if (memo.next_funding > ts_event) {
        out.next_funding_time = memo.next_funding;
    }
    out.interval_s = k_funding_interval_s;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireMarket& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto base_volume = parse_amount_e9(wire.volume_base);
    const auto quote_volume = parse_turnover_e6(wire.volume_quote);
    if (!base_volume.has_value() && !quote_volume.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    if (base_volume.has_value()) {
        out.volume_24h_base = *base_volume;
    }
    if (quote_volume.has_value()) {
        out.volume_24h_quote = *quote_volume;
    }
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::extended
