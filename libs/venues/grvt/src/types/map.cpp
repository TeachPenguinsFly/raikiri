#include <raikiri/book/ladder.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/grvt/map.hpp>

#include <variant>

namespace raikiri::venues::grvt {

namespace {

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return book::exact_steps(*value, step);
}

// Summing two stated halves; either one missing leaves the total unstated.
FixedE9 sum_e9(std::string_view a, std::string_view b) noexcept {
    const auto x = parse_decimal_e9(a);
    const auto y = parse_decimal_e9(b);
    if (!x.has_value() || !y.has_value()) {
        return FixedE9::undef();
    }
    return *x + *y;
}

} // namespace

std::optional<PriceTicks> to_ticks(std::string_view text, FixedE9 tick) noexcept {
    const auto steps = steps_of(text, tick);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, FixedE9 lot) noexcept {
    const auto steps = steps_of(text, lot);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

FixedE9 percent_to_ratio(std::string_view text) noexcept {
    if (text.empty()) {
        return FixedE9::undef();
    }
    const auto percent = parse_rate_e9(text);
    if (!percent.has_value()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(percent->raw / 100);
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    // Every row GRVT lists is a USDT-margined perpetual: 194 of 194 on
    // 2026-09-24, active or delisted, and no spot, future or option kind
    // answered a filtered query.
    if (market != MarketType::Perp || wire.kind != "PERPETUAL" || wire.status != "ACTIVE") {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.min_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.instrument);
    const auto canonical = canonical_symbol(wire.base, wire.quote);
    if (!symbol.has_value() || !canonical.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Grvt, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto notional = parse_decimal_e9(wire.min_notional); notional.has_value()) {
        base.min_notional = *notional;
    }
    // Sizes are in the base asset: one lot of BTC_USDT_Perp is 0.001 BTC.
    base.contract_value = FixedE9::from_raw(1'000'000'000);
    base.kind = ContractKind::Linear;

    PerpInstrument perp{};
    perp.base = base;
    // THE VENUE STATES IT PER CONTRACT, and it is not one number: 124 of the
    // 184 settle every eight hours and 60 every four. The funding history
    // agrees row for row — BTC's last eight settlements 8 h apart, ASTER's
    // and BERA's 4 h apart, read 2026-09-24. 0 stays 0: a row that states no
    // interval is one the carry board must refuse, not one that settles on a
    // default.
    perp.funding_interval_s = wire.funding_interval_hours * 3600U;
    perp.funding_cap = percent_to_ratio(wire.funding_cap);
    perp.funding_floor = percent_to_ratio(wire.funding_floor);
    return perp;
}

std::optional<TradeTick> map_trade(const WirePrint& wire,
                                   const InstrumentBase& base,
                                   UnixNanos ts_event,
                                   UnixNanos ts_init) {
    const auto price = to_ticks(wire.price, base.tick_size);
    const auto qty = to_lots(wire.size, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = wire.taker_bought ? Side::Buy : Side::Sell;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireTicker& wire,
                                         const Instrument& instrument,
                                         UnixNanos ts_event,
                                         UnixNanos ts_init) {
    const auto rate = percent_to_ratio(wire.funding_rate);
    if (rate.is_undef()) {
        return std::nullopt;
    }
    const auto* perp = std::get_if<PerpInstrument>(&instrument);
    if (perp == nullptr) {
        return std::nullopt;
    }
    const auto& base = perp->base;
    FundingUpdate out{};
    out.instrument = base.id;
    // The rate accruing now, settled at next_funding_time. GRVT publishes no
    // separate estimate of the one after, so predicted_rate stays UNDEF.
    out.rate = rate;
    // The mark carries nine decimals and sits off the tick grid, so it is
    // truncated onto it rather than refused.
    if (const auto mark = parse_decimal_e9(wire.mark_price);
        mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    out.next_funding_time = UnixNanos{wire.next_funding_ns};
    out.interval_s = wire.funding_interval_hours != 0 ? wire.funding_interval_hours * 3600U
                                                      : perp->funding_interval_s;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTicker& wire,
                                            const InstrumentBase& base,
                                            UnixNanos ts_event,
                                            UnixNanos ts_init) {
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = sum_e9(wire.buy_volume_base, wire.sell_volume_base);
    out.volume_24h_quote = sum_e9(wire.buy_volume_quote, wire.sell_volume_quote);
    if (out.volume_24h_base.is_undef() && out.volume_24h_quote.is_undef()) {
        return std::nullopt;
    }
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::grvt
