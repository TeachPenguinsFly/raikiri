#include <raikiri/venues/bingx/map.hpp>

#include <optional>

namespace raikiri::venues::bingx {

namespace {

// FixedE9 holds nine fractional digits, so a precision past nine has no exact
// step and the row is refused rather than approximated.
constexpr std::int64_t k_max_precision = 9;

constexpr std::uint32_t k_seconds_per_hour = 3600;

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means BingX changed a contract's precision, so the message is
// dropped rather than rounded — rounding is how an order ends up one tick away
// from the intended price.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

std::optional<PriceTicks> to_ticks(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

// Rates are venue-scale decimals, not instrument-scaled, so they keep their
// 1e9 representation rather than becoming ticks.
std::optional<FixedE9> to_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? std::optional{*value} : std::nullopt;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept {
    if (precision < 0 || precision > k_max_precision) {
        return std::nullopt;
    }
    std::int64_t divisor = 1;
    for (std::int64_t i = 0; i < precision; ++i) {
        divisor *= 10;
    }
    return FixedE9::from_raw(k_e9 / divisor);
}

std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.bid_price, base);
    const auto ask = to_ticks(wire.ask_price, base);
    const auto bid_qty = to_lots(wire.bid_qty, base);
    const auto ask_qty = to_lots(wire.ask_qty, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    // T, not E: the transaction time is when the book actually changed, while
    // E is when BingX got around to sending it. Using E would fold venue
    // queueing delay into our own latency numbers.
    out.ts_event = ms_to_unix(wire.transaction_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
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
    // The flip: `m` says the BUYER was the maker, so the aggressor sold.
    out.aggressor = wire.buyer_is_maker ? Side::Sell : Side::Buy;
    out.ts_event = ms_to_unix(wire.trade_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WirePremiumIndex& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.last_funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    // BingX rounds the mark to the contract's own pricePrecision, so the exact
    // rule holds here where binance needs an exemption. A mark that fails to
    // divide leaves the field undefined rather than discarding the rate.
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    // predicted_rate stays undefined: BingX publishes no next-interval
    // forecast, and echoing the current rate would fabricate one.
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    if (wire.funding_interval_hours > 0) {
        // Stated per contract, unlike okx where it must be inferred from the
        // gap between two settlement times.
        out.interval_s =
            static_cast<std::uint32_t>(wire.funding_interval_hours) * k_seconds_per_hour;
    }
    // This arrives as a REST answer, and the body's only timestamp is
    // updateTime — the settlement it last charged, not when it was published.
    // Ingest time is the honest ts_event.
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    // 1 is online; 25 covers listed-but-not-trading rows (165 of 1021 today,
    // all synthetic NC-prefixed commodity contracts). They ship complete
    // precisions, so only this check keeps them out.
    if (wire.status != 1) {
        return std::nullopt;
    }
    // /quote/contracts is the perpetual catalogue and nothing else; spot lives
    // under a different API family this adapter does not speak.
    if (market != MarketType::Perp) {
        return std::nullopt;
    }

    const auto tick = step_from_precision(wire.price_precision);
    const auto lot = step_from_precision(wire.quantity_precision);
    if (!tick.has_value() || !lot.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Bingx, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *parse_decimal_e9("1");
    // tradeMinUSDT is a genuine notional floor (2 on every row today), unlike
    // okx's minSz, which counts contracts. Left undefined rather than zeroed
    // when unparseable: zero reads as "no minimum".
    if (const auto min_notional = to_e9(wire.trade_min_usdt); min_notional.has_value()) {
        base.min_notional = *min_notional;
    }
    // Zero on purpose: the catalogue does not carry the interval. The
    // premiumIndex poll states it per contract and map_funding attaches it.
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::bingx
