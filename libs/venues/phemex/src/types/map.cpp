#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/phemex/map.hpp>

#include <array>
#include <limits>
#include <string>
#include <unordered_map>

namespace raikiri::venues::phemex {

namespace {

constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;

constexpr std::array<std::int64_t, 10> k_pow10{
    1, 10, 100, 1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000, 1'000'000'000};

// "1000 SHIB" -> "1000SHIB".
std::string without_spaces(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != ' ') {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<InstrumentBase> base_row(MarketType market,
                                       std::string_view symbol,
                                       std::string_view base_asset,
                                       std::string_view quote_asset) {
    const auto exchange = SymbolText::from(symbol);
    const auto local = canonical_symbol(without_spaces(base_asset), quote_asset);
    if (!exchange.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Phemex, market, SymbolId{0});
    base.exchange_symbol = *exchange;
    base.local_symbol = *local;
    return base;
}

std::optional<PriceTicks> mark_ticks(std::optional<FixedE9> mark, FixedE9 tick) {
    if (!mark.has_value() || tick.raw <= 0) {
        return std::nullopt;
    }
    // The mark is an index average and lands between ticks.
    return PriceTicks::from_raw(mark->raw / tick.raw);
}

} // namespace

std::optional<std::int64_t> real_steps(std::string_view text, FixedE9 step) noexcept {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<std::int64_t> scaled_steps(std::int64_t value, std::int64_t step) noexcept {
    if (step <= 0 || value % step != 0) {
        return std::nullopt;
    }
    return value / step;
}

std::optional<FixedE9> scaled_e9(std::int64_t value, std::uint32_t scale) noexcept {
    if (scale <= 9) {
        const auto factor = k_pow10[9 - scale];
        if (value > std::numeric_limits<std::int64_t>::max() / factor ||
            value < -(std::numeric_limits<std::int64_t>::max() / factor)) {
            return std::nullopt;
        }
        return FixedE9::from_raw(value * factor);
    }
    if (scale > 18) {
        return std::nullopt;
    }
    const auto divisor = k_pow10[scale - 9];
    if (value % divisor != 0) {
        return std::nullopt;
    }
    return FixedE9::from_raw(value / divisor);
}

UnixNanos ns_to_unix(std::uint64_t ns, UnixNanos fallback) noexcept {
    return ns == 0 ? fallback : UnixNanos{ns};
}

UnixNanos next_funding_time(UnixNanos now, std::uint32_t interval_s) noexcept {
    if (interval_s == 0) {
        return UnixNanos{};
    }
    const auto interval_ns = std::uint64_t{interval_s} * k_ns_per_s;
    return UnixNanos{(now.ns / interval_ns + 1) * interval_ns};
}

std::optional<Listed> map_linear_perp(const WirePerp& wire) {
    // fundingInterval is authoritative; the description text can disagree
    if (wire.status != "Listed" || wire.funding_interval_s == 0) {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.qty_step);
    auto base = base_row(MarketType::Perp, wire.symbol, wire.base, wire.quote);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 ||
        !base.has_value()) {
        return std::nullopt;
    }
    base->tick_size = *tick;
    base->lot_size = *lot;
    if (const auto floor = parse_decimal_e9(wire.min_order_value); floor.has_value()) {
        base->min_notional = *floor;
    }
    base->contract_value = FixedE9::from_raw(k_e9);
    base->kind = ContractKind::Linear;
    Listing listing{};
    listing.encoding = Encoding::Real;
    listing.funding_interval_s = wire.funding_interval_s;
    return Listed{PerpInstrument{*base, wire.funding_interval_s}, listing};
}

std::optional<Listed> map_inverse_perp(const WireProduct& wire, std::uint32_t settle_scale) {
    if (wire.type != "Perpetual" || wire.status != "Listed" || wire.funding_interval_s == 0 ||
        wire.price_scale == 0 || wire.price_scale > 9 || wire.lot_size <= 0) {
        return std::nullopt;
    }
    const auto tick = parse_number_e9(wire.tick_size);
    const auto contract = parse_number_e9(wire.contract_size);
    auto base = base_row(MarketType::Perp, wire.symbol, wire.settle, wire.quote);
    if (!tick.has_value() || !contract.has_value() || tick->raw <= 0 || contract->raw <= 0 ||
        !base.has_value()) {
        return std::nullopt;
    }
    const auto per_ep = k_pow10[9 - wire.price_scale];
    if (tick->raw % per_ep != 0) {
        return std::nullopt;
    }
    base->tick_size = *tick;
    // Quantities are whole contracts, each worth `contract_value` of the quote.
    base->lot_size = FixedE9::from_raw(wire.lot_size * k_e9);
    base->contract_value = *contract;
    base->kind = ContractKind::Inverse;
    Listing listing{};
    listing.encoding = Encoding::Scaled;
    listing.price_step = tick->raw / per_ep;
    listing.qty_step = wire.lot_size;
    listing.price_scale = wire.price_scale;
    listing.ratio_scale = wire.ratio_scale;
    listing.turnover_scale = settle_scale;
    listing.funding_interval_s = wire.funding_interval_s;
    return Listed{PerpInstrument{*base, wire.funding_interval_s}, listing};
}

std::optional<Listed>
map_spot(const WireProduct& wire, std::uint32_t base_scale, std::uint32_t quote_scale) {
    if (wire.type != "Spot" || wire.status != "Listed" || wire.price_scale == 0 ||
        wire.quote_tick_ev <= 0 || wire.base_tick_ev <= 0) {
        return std::nullopt;
    }
    const auto tick = scaled_e9(wire.quote_tick_ev, wire.price_scale);
    const auto lot = scaled_e9(wire.base_tick_ev, base_scale);
    auto base = base_row(MarketType::Spot, wire.symbol, wire.base, wire.quote);
    if (!tick.has_value() || !lot.has_value() || !base.has_value()) {
        return std::nullopt;
    }
    base->tick_size = *tick;
    base->lot_size = *lot;
    if (const auto floor = scaled_e9(wire.min_order_value_ev, quote_scale);
        floor.has_value() && wire.min_order_value_ev > 0) {
        base->min_notional = *floor;
    }
    if (wire.has_fees) {
        const auto taker = scaled_e9(wire.taker_fee_er, wire.ratio_scale);
        const auto maker = scaled_e9(wire.maker_fee_er, wire.ratio_scale);
        if (taker.has_value() && maker.has_value()) {
            base->taker_fee = *taker;
            base->maker_fee = *maker;
        }
    }
    base->contract_value = FixedE9::from_raw(k_e9);
    base->kind = ContractKind::Linear;
    Listing listing{};
    listing.encoding = Encoding::Scaled;
    listing.price_step = wire.quote_tick_ev;
    listing.qty_step = wire.base_tick_ev;
    listing.price_scale = wire.price_scale;
    listing.ratio_scale = wire.ratio_scale;
    listing.volume_scale = base_scale;
    listing.turnover_scale = quote_scale;
    return Listed{SpotInstrument{*base}, listing};
}

MappedCatalogue map_catalogue(const WireCatalogue& wire, MarketType market) {
    std::unordered_map<std::string_view, std::uint32_t> scales;
    for (const auto& currency : wire.currencies) {
        scales.emplace(currency.currency, currency.value_scale);
    }
    const auto scale_of = [&](std::string_view code) -> std::optional<std::uint32_t> {
        const auto found = scales.find(code);
        return found == scales.end() ? std::nullopt : std::optional{found->second};
    };

    MappedCatalogue out{};
    const auto keep = [&](std::optional<Listed> listed) {
        if (listed.has_value()) {
            out.listed.push_back(std::move(*listed));
        } else {
            ++out.refused;
        }
    };
    if (market == MarketType::Perp) {
        for (const auto& row : wire.perps) {
            if (row.status == "Listed") {
                keep(map_linear_perp(row));
            }
        }
    }
    for (const auto& row : wire.products) {
        if (row.status != "Listed") {
            continue;
        }
        if (market == MarketType::Perp && row.type == "Perpetual") {
            const auto settle = scale_of(row.settle);
            keep(settle.has_value() ? map_inverse_perp(row, *settle) : std::nullopt);
        } else if (market == MarketType::Spot && row.type == "Spot") {
            const auto base = scale_of(row.base);
            const auto quote = scale_of(row.quote);
            keep(base.has_value() && quote.has_value() ? map_spot(row, *base, *quote)
                                                       : std::nullopt);
        }
    }
    return out;
}

std::optional<std::size_t> to_levels(std::span<const WireRealLevel> wire,
                                     const InstrumentBase& base,
                                     std::span<book::Level> out) {
    if (wire.size() > out.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < wire.size(); ++i) {
        const auto price = real_steps(wire[i].price, base.tick_size);
        const auto qty = real_steps(wire[i].qty, base.lot_size);
        if (!price.has_value() || !qty.has_value()) {
            return std::nullopt;
        }
        out[i] = {PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};
    }
    return wire.size();
}

std::optional<std::size_t> to_levels(std::span<const WireScaledLevel> wire,
                                     const Listing& listing,
                                     std::span<book::Level> out) {
    if (wire.size() > out.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < wire.size(); ++i) {
        const auto price = scaled_steps(wire[i].price_ep, listing.price_step);
        const auto qty = scaled_steps(wire[i].qty_ev, listing.qty_step);
        if (!price.has_value() || !qty.has_value()) {
            return std::nullopt;
        }
        out[i] = {PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};
    }
    return wire.size();
}

std::optional<TradeTick>
map_trade(const WireRealPrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = real_steps(wire.price, base.tick_size);
    const auto qty = real_steps(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.taker_bought ? Side::Buy : Side::Sell;
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireScaledPrint& wire,
                                   const InstrumentBase& base,
                                   const Listing& listing,
                                   UnixNanos ts_init) {
    const auto price = scaled_steps(wire.price_ep, listing.price_step);
    const auto qty = scaled_steps(wire.qty_ev, listing.qty_step);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.taker_bought ? Side::Buy : Side::Sell;
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WirePackRow& wire,
                                         const InstrumentBase& base,
                                         const Listing& listing,
                                         std::uint64_t ts_ns,
                                         UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto predicted = parse_rate_e9(wire.predicted); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    if (const auto mark = mark_ticks(parse_number_e9(wire.mark), base.tick_size);
        mark.has_value()) {
        out.mark_price = *mark;
    }
    out.ts_event = ns_to_unix(ts_ns, ts_init);
    out.interval_s = listing.funding_interval_s;
    out.next_funding_time = next_funding_time(out.ts_event, listing.funding_interval_s);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireInverseTicker& wire,
                                         const InstrumentBase& base,
                                         const Listing& listing,
                                         UnixNanos ts_init) {
    if (!wire.rate_er.has_value()) {
        return std::nullopt;
    }
    const auto rate = scaled_e9(*wire.rate_er, listing.ratio_scale);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (wire.predicted_er.has_value()) {
        if (const auto predicted = scaled_e9(*wire.predicted_er, listing.ratio_scale);
            predicted.has_value()) {
            out.predicted_rate = *predicted;
        }
    }
    if (wire.mark_ep.has_value() && listing.price_step > 0) {
        out.mark_price = PriceTicks::from_raw(*wire.mark_ep / listing.price_step);
    }
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.interval_s = listing.funding_interval_s;
    out.next_funding_time = next_funding_time(out.ts_event, listing.funding_interval_s);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WirePackRow& wire,
                                            const InstrumentBase& base,
                                            std::uint64_t ts_ns,
                                            UnixNanos ts_init) {
    const auto volume = parse_amount_e9(wire.volume);
    const auto turnover = parse_turnover_e6(wire.turnover);
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
    out.ts_event = ns_to_unix(ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireInverseTicker& wire,
                                            const InstrumentBase& base,
                                            const Listing& listing,
                                            UnixNanos ts_init) {
    // Inverse: `volume` counts contracts (the quote), `turnover` the settle coin (the base).
    std::optional<FixedE9> coins;
    if (wire.turnover_ev.has_value()) {
        coins = scaled_e9(*wire.turnover_ev, listing.turnover_scale);
    }
    std::optional<FixedE6> quote;
    if (wire.volume.has_value() && base.contract_value.raw > 0) {
        const auto units = mul_wide(*wire.volume, base.contract_value.raw).raw / (k_e9 / k_e6);
        if (units <= std::numeric_limits<std::int64_t>::max() &&
            units >= std::numeric_limits<std::int64_t>::min()) {
            quote = FixedE6::from_raw(static_cast<std::int64_t>(units));
        }
    }
    if (!coins.has_value() && !quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    if (coins.has_value()) {
        out.volume_24h_base = *coins;
    }
    if (quote.has_value()) {
        out.volume_24h_quote = *quote;
    }
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireSpotTicker& wire,
                                            const InstrumentBase& base,
                                            const Listing& listing,
                                            UnixNanos ts_init) {
    std::optional<FixedE9> volume;
    std::optional<FixedE6> turnover;
    if (wire.volume_ev.has_value()) {
        volume = scaled_e9(*wire.volume_ev, listing.volume_scale);
    }
    if (wire.turnover_ev.has_value()) {
        if (listing.turnover_scale >= 6 && listing.turnover_scale <= 15) {
            turnover = FixedE6::from_raw(*wire.turnover_ev / k_pow10[listing.turnover_scale - 6]);
        } else if (const auto whole = scaled_e9(*wire.turnover_ev, listing.turnover_scale)) {
            turnover = FixedE6::from_raw(whole->raw / (k_e9 / k_e6));
        }
    }
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
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::phemex
