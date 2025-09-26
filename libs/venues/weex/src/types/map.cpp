#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/weex/map.hpp>

#include <optional>

namespace raikiri::venues::weex {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means WEEX changed a precision, so the message is dropped
// rather than rounded — rounding is how a price ends up one tick from where the
// venue put it.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return book::exact_steps(*value, step);
}

std::optional<PriceTicks> to_ticks(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

// The price step WEEX implies by stating a precision. Nullopt past 1e-9, which
// is two live rows: a step FixedE9 cannot hold divides nothing exactly, so the
// contract is refused at load rather than silently dropping every level later.
std::optional<FixedE9> step_from_precision(std::uint64_t precision) noexcept {
    if (precision > 9) {
        return std::nullopt;
    }
    std::int64_t raw = 1;
    for (std::uint64_t i = precision; i < 9; ++i) {
        raw *= 10;
    }
    return FixedE9::from_raw(raw);
}

// Seconds between settlements, from how many times a day the contract settles.
// Nullopt for a timetable that does not divide the day — WEEX lists 3, 6 and 24
// entries and nothing else, and a count that did not divide would name an
// interval no two settlements are actually apart.
std::optional<std::uint32_t> interval_from(std::uint32_t settlements_per_day) noexcept {
    constexpr std::uint32_t k_seconds_per_day = 86'400;
    if (settlements_per_day == 0 || k_seconds_per_day % settlements_per_day != 0) {
        return std::nullopt;
    }
    return k_seconds_per_day / settlements_per_day;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<TradeTick> map_trade(const WirePrint& wire,
                                   std::string_view symbol,
                                   const exec::InstrumentTable& table,
                                   UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    // THE FLIP: `m` says the BUYER was the maker, so the aggressor SOLD.
    // Correlated against the live 15-level book over 75 s: `m: true` printed at
    // or below the best bid 372 times and at or above the best ask 33, `m:
    // false` at or above the ask 596 times and at or below the bid 29 — the
    // minorities are the book being a frame behind the print. Inverted, every
    // signed-flow series built on this venue runs backwards and nothing says so.
    out.aggressor = wire.buyer_is_maker ? Side::Sell : Side::Buy;
    // The print's own T, not the frame's E: one frame carries several prints
    // and they did not all happen when WEEX sent them.
    out.ts_event = ms_to_unix(wire.trade_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.volume.empty() && wire.quote_volume.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    // A turnover is an AMOUNT and not a price: WEEX states it at its own
    // resolution and an exact read would refuse the whole figure rather than
    // its tail, leaving a traded market showing nothing. A column the frame did
    // not state stays UNDEF — which is not the claim that it traded zero.
    if (const auto base_volume = parse_amount_e9(wire.volume); base_volume.has_value()) {
        out.volume_24h_base = *base_volume;
    }
    if (const auto quote_volume = parse_amount_e9(wire.quote_volume); quote_volume.has_value()) {
        out.volume_24h_quote = *quote_volume;
    }
    out.ts_event = ms_to_unix(wire.close_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WirePremium& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    if (wire.funding_rate.empty()) {
        return std::nullopt; // quotes no funding: not the same as funding at zero
    }
    // TRUNCATING, because 349 of 1022 rows pad the rate past 1e-9 and an exact
    // read refuses every one of them.
    const auto rate = parse_rate_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto forecast = parse_rate_e9(wire.forecast_rate); forecast.has_value()) {
        out.predicted_rate = *forecast; // absent leaves it UNDEF, never zero
    }
    // EXACT OR UNDEF. It divided by the contract's tick on 1022 of 1022 rows,
    // so nothing is lost today; a mark rounded into the nearest tick would be a
    // price the venue never quoted, and this field is what a basis is measured
    // against.
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    out.interval_s = std::visit(
        [](const auto& concrete) -> std::uint32_t {
            if constexpr (requires { concrete.funding_interval_s; }) {
                return concrete.funding_interval_s;
            } else {
                return 0;
            }
        },
        *instrument);
    out.ts_event = ms_to_unix(wire.event_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    // PERPETUALS ONLY, which is everything WEEX lists on this tree: all 1022
    // rows are USDT-margined and USDT-quoted, and there is no dated contract
    // and no spot pair among them. Admitting a Spot id here would intern the
    // whole perpetual catalogue as spot and look like it worked.
    switch (market) {
    case MarketType::Perp:
        break;
    case MarketType::Spot:
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    }
    // TRADIFI_PERPETUAL is a perpetual on a non-crypto underlying and settles,
    // funds and margins exactly as PERPETUAL does; refusing it would drop 446
    // tradable contracts for the sake of what their index tracks.
    if (wire.contract_type != "PERPETUAL" && wire.contract_type != "TRADIFI_PERPETUAL") {
        return std::nullopt;
    }

    const auto tick = step_from_precision(wire.price_precision);
    const auto lot = parse_decimal_e9(wire.contract_val);
    if (!tick.has_value() || !lot.has_value() || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto interval = interval_from(wire.settlements_per_day);
    if (!interval.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }
    // A row whose pair WEEX did not state is DROPPED rather than carried under
    // the exchange symbol: an instrument nothing can group shows in a
    // cross-venue view as a market of its own.
    const auto canonical = canonical_symbol(wire.base_asset, wire.quote_asset);
    if (!canonical.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Weex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    // ONE BASE UNIT PER UNIT OF SIZE. Quantities on this tree are base amounts,
    // not contract counts: the ticker's `v` times its own weighted average `w`
    // reproduced its `q` to five figures, and the depth quantities share that
    // scale. `contractVal` is the size STEP here, and it is already lot_size.
    base.contract_value = FixedE9::from_raw(1'000'000'000);
    base.kind = ContractKind::Linear;
    // min_notional and the fees stay UNDEF. WEEX states no notional minimum at
    // all, and the fee rates it does state are the public tier's — 11 rows carry
    // a separate apiMakerFeeRate, which is the proof that the public number is
    // not necessarily the one an account pays.
    return PerpInstrument{base, *interval};
}

} // namespace raikiri::venues::weex
