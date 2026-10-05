#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/edgex/map.hpp>

namespace raikiri::venues::edgex {

namespace {

constexpr std::size_t k_e9_places = 9;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<FixedE9> positive(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw <= 0) {
        return std::nullopt;
    }
    return *value;
}

UnixNanos from_ms(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

} // namespace

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    if (!wire.enabled || (market != MarketType::Perp && market != MarketType::Spot)) {
        return std::nullopt;
    }
    const auto tick = positive(wire.tick_size);
    const auto lot = positive(wire.step_size);
    const auto symbol = SymbolText::from(wire.name);
    const auto local = canonical_symbol(wire.base, wire.quote);
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Edgex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    if (market == MarketType::Spot) {
        if (const auto floor = parse_decimal_e9(wire.min_order_value); floor.has_value()) {
            base.min_notional = *floor;
        }
        return SpotInstrument{base};
    }
    PerpInstrument perp{base, wire.funding_interval_min * 60U};
    if (const auto cap = parse_rate_e9(wire.funding_max); cap.has_value()) {
        perp.funding_cap = *cap;
    }
    if (const auto floor = parse_rate_e9(wire.funding_min); floor.has_value()) {
        perp.funding_floor = *floor;
    }
    return perp;
}

PriceTicks mark_ticks(std::string_view text, FixedE9 tick_size) noexcept {
    if (const auto dot = text.find('.'); dot != std::string_view::npos && text.size() - dot - 1 > k_e9_places) {
        text = text.substr(0, dot + 1 + k_e9_places);
    }
    const auto mark = parse_decimal_e9(text);
    if (!mark.has_value() || mark->raw <= 0 || tick_size.raw <= 0) {
        return PriceTicks{};
    }
    return PriceTicks::from_raw(mark->raw / tick_size.raw);
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
    out.aggressor = wire.buyer_maker ? Side::Sell : Side::Buy;
    out.ts_event = from_ms(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireTicker& wire, const Instrument& instrument, UnixNanos ts_init) {
    const auto* perp = std::get_if<PerpInstrument>(&instrument);
    if (perp == nullptr || wire.funding_rate.empty()) {
        return std::nullopt;
    }
    const auto rate = parse_rate_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = perp->base.id;
    // the forecast rate is what accrues now; predictedFundingRate is a constant interest leg
    out.rate = *rate;
    out.mark_price = mark_ticks(wire.mark_price, perp->base.tick_size);
    out.next_funding_time = from_ms(wire.next_funding_ms, UnixNanos{0});
    out.interval_s = perp->funding_interval_s;
    out.ts_event = UnixNanos{};
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto volume = wire.volume_base.empty() ? std::nullopt : parse_amount_e9(wire.volume_base);
    const auto turnover = wire.volume_quote.empty() ? std::nullopt : parse_turnover_e6(wire.volume_quote);
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
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::edgex
