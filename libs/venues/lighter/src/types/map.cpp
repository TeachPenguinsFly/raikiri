#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/lighter/map.hpp>

#include <optional>

namespace raikiri::venues::lighter {

namespace {

constexpr std::uint64_t k_interval_ms = std::uint64_t{k_funding_interval_s} * 1000;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

// The stats frame writes every rate in PERCENT. Exact or refused: four places
// of a percent is 1e-6, far inside what e9 holds, so a remainder here means
// the venue changed how it writes the field.
std::optional<FixedE9> percent(std::string_view text, std::int64_t divisor) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw % divisor != 0) {
        return std::nullopt;
    }
    return FixedE9::from_raw(value->raw / divisor);
}

std::optional<SymbolText> local_symbol(const WireMarket& wire, MarketType market) {
    if (market == MarketType::Perp) {
        return canonical_symbol(wire.symbol, k_perp_quote);
    }
    const auto halves = split_symbol(wire.symbol, '/');
    if (!halves.has_value()) {
        return std::nullopt;
    }
    return canonical_symbol(halves->first.view(), halves->second.view());
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_of(std::uint32_t decimals) noexcept {
    if (decimals > 9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::uint32_t i = 0; i < decimals; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    std::string_view wanted;
    switch (market) {
    case MarketType::Perp:
        wanted = "perp";
        break;
    case MarketType::Spot:
        wanted = "spot";
        break;
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    }
    if (wire.market_type != wanted || wire.status != "active") {
        return std::nullopt;
    }
    const auto tick = step_of(wire.price_decimals);
    const auto lot = step_of(wire.size_decimals);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = local_symbol(wire, market);
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Lighter, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto floor = parse_decimal_e9(wire.min_quote_amount); floor.has_value()) {
        base.min_notional = *floor;
    }
    // The size is the coin itself on both markets. The `multiplier` a spot row
    // carries is its TOKEN's — rhSPY states 1.0017 in /api/v1/assetDetails too
    // — and says nothing about what one unit of size buys.
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }

    PerpInstrument perp{base, k_funding_interval_s};
    // funding_clamp_big is a percent PER EIGHT HOURS ("BigClamp limits funding
    // payments to 4% per 8 hours, or 0.5% per hour"), and the hourly rate is
    // clamped to it before the division by eight — so the cap on the rate this
    // adapter publishes is clamp / 8 / 100. Symmetric by the same formula.
    if (const auto cap = percent(wire.funding_clamp_big, 800); cap.has_value()) {
        perp.funding_cap = *cap;
        perp.funding_floor = FixedE9::from_raw(-cap->raw);
    }
    return perp;
}

std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.size, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    // The maker rested on the ask, so the taker lifted it.
    out.aggressor = wire.maker_is_ask ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireMarketStats& wire,
                                         const InstrumentBase& base,
                                         std::uint64_t frame_ms,
                                         UnixNanos ts_init) {
    if (wire.current_funding_rate.empty()) {
        return std::nullopt;
    }
    const auto rate = percent(wire.current_funding_rate, 100);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    // predicted_rate stays UNDEF. The frame's other rate, `funding_rate`, is
    // the one ALREADY PAID at the last hour mark — a fact about the past, not
    // a forecast of the hour after this one, which the venue does not publish.
    if (const auto mark = steps_of(wire.mark_price, base.tick_size); mark.has_value()) {
        out.mark_price = PriceTicks::from_raw(*mark);
    }
    // The next hour mark after the last settlement. A market that has not
    // settled within the hour — a stale timestamp months old is what an idle
    // market states — gets no next time rather than one in the past.
    if (wire.funding_ms != 0) {
        const auto next = (wire.funding_ms / k_interval_ms + 1) * k_interval_ms;
        if (next > frame_ms) {
            out.next_funding_time = ms_to_unix(next, UnixNanos{0});
        }
    }
    out.interval_s = k_funding_interval_s;
    out.ts_event = ms_to_unix(frame_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireMarketStats& wire,
                                            const InstrumentBase& base,
                                            std::uint64_t frame_ms,
                                            UnixNanos ts_init) {
    // parse_amount_e9 because these are JSON floats as a Go encoder writes
    // them — 89.40990000000001 is a live value — and an exact read would
    // refuse the whole figure over its last binary digit.
    const auto volume = parse_amount_e9(wire.base_volume);
    const auto turnover = parse_amount_e9(wire.quote_volume);
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
    out.ts_event = ms_to_unix(frame_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::lighter
