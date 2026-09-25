#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/backpack/map.hpp>

#include <optional>

namespace raikiri::venues::backpack {

namespace {

using Level = book::Level;

// exact or nothing; never rounded
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<PriceTicks> to_ticks(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> resting_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = tradable_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

// undef when absent: zero would claim the market did not trade
FixedE9 amount_or_undef(std::string_view text) {
    if (text.empty()) {
        return FixedE9::undef();
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? *value : FixedE9::undef();
}

std::uint32_t interval_of(const Instrument& instrument) {
    const auto* perp = std::get_if<PerpInstrument>(&instrument);
    return perp != nullptr ? perp->funding_interval_s : 0;
}

} // namespace

UnixNanos us_to_unix(std::uint64_t us, UnixNanos fallback) noexcept {
    return us == 0 ? fallback : UnixNanos{us * 1'000ULL};
}

std::optional<Bbo> map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.bid_price, base);
    const auto ask = to_ticks(wire.ask_price, base);
    const auto bid_qty = resting_lots(wire.bid_qty, base);
    const auto ask_qty = resting_lots(wire.ask_qty, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = us_to_unix(wire.engine_us, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Bbo> map_touch(const WireDepthSnapshot& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto best = [&](std::span<const WireLevel> side, bool highest) -> std::optional<Level> {
        std::optional<Level> out;
        for (const auto& level : side) {
            const auto price = to_ticks(level.price, base);
            const auto qty = resting_lots(level.qty, base);
            if (!price.has_value() || !qty.has_value()) {
                return std::nullopt; // off-grid: no touch rather than a wrong one
            }
            if (!out.has_value() || (highest ? price->raw > out->price.raw : price->raw < out->price.raw)) {
                out = Level{.price = *price, .qty = *qty};
            }
        }
        return out;
    };
    const auto bid = best(wire.bids, true);
    const auto ask = best(wire.asks, false);
    if (!bid.has_value() || !ask.has_value()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = base.id;
    out.bid = bid->price;
    out.ask = ask->price;
    out.bid_qty = bid->qty;
    out.ask_qty = ask->qty;
    out.ts_event = us_to_unix(wire.timestamp_us, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    // exact: a floored print would be a size that never traded
    const auto qty = to_lots(wire.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = wire.buyer_is_maker ? Side::Sell : Side::Buy;
    out.ts_event = us_to_unix(wire.engine_us, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.funding_rate.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    // truncated: the rate carries up to 28 fractional digits
    const auto rate = parse_rate_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = wire.next_funding_ms == 0 ? UnixNanos{0} : UnixNanos{wire.next_funding_ms * 1'000'000ULL};
    out.interval_s = interval_of(*instrument);
    out.ts_event = us_to_unix(wire.event_us, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos received) {
    if (wire.volume.empty() && wire.quote_volume.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = amount_or_undef(wire.volume);
    out.volume_24h_quote = parse_turnover_e6(wire.quote_volume).value_or(FixedE6::undef());
    out.ts_event = received;
    out.ts_init = received;
    return out;
}

namespace {

AssetClass asset_class_of(std::string_view rwa_market_type) noexcept {
    if (rwa_market_type.empty()) {
        return AssetClass::Crypto;
    }
    if (rwa_market_type == "STOCK") {
        return AssetClass::Equity;
    }
    if (rwa_market_type == "INDEX") {
        return AssetClass::Index;
    }
    return AssetClass::Unknown;
}

} // namespace

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    const bool perp = wire.market_type == "PERP";
    const bool spot = wire.market_type == "SPOT";
    switch (market) {
    case MarketType::Perp:
        if (!perp) {
            return std::nullopt;
        }
        break;
    case MarketType::Spot:
        if (!spot) {
            return std::nullopt;
        }
        break;
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    }
    if (wire.order_book_state != "Open") {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.step_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    const auto canonical = canonical_symbol(wire.base_asset, wire.quote_asset);
    if (!symbol.has_value() || !canonical.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Backpack, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    base.asset_class = asset_class_of(wire.rwa_market_type);
    if (spot) {
        return SpotInstrument{base};
    }
    // a non-whole-second interval stays 0; caps stay undef since the venue's bounds state no unit
    const auto interval_s = wire.funding_interval_ms % 1000 == 0 ? wire.funding_interval_ms / 1000 : 0;
    PerpInstrument out{};
    out.base = base;
    out.funding_interval_s = static_cast<std::uint32_t>(interval_s);
    return out;
}

} // namespace raikiri::venues::backpack
