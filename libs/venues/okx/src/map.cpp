#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/okx/map.hpp>

#include <optional>

namespace raikiri::venues::okx {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means OKX changed an instrument's precision, so the message is
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

// ctMult is a plain count, not a decimal quantity; anything fractional would
// make the contract size a non-integer multiple of ctVal, which OKX does not
// define, so the row is refused instead of approximated.
std::optional<std::int64_t> whole_number(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw <= 0 || value->raw % k_e9 != 0) {
        return std::nullopt;
    }
    return value->raw / k_e9;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
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
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
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
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base_of(*instrument).id;
    out.rate = *rate;
    if (const auto predicted = to_e9(wire.next_funding_rate); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    out.next_funding_time = ms_to_unix(wire.funding_ms, UnixNanos{0});
    if (wire.next_funding_ms > wire.funding_ms && wire.funding_ms != 0) {
        out.interval_s =
            static_cast<std::uint32_t>((wire.next_funding_ms - wire.funding_ms) / 1000);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (wire.state != "live") {
        return std::nullopt;
    }
    if (market == MarketType::Spot) {
        if (wire.inst_type != "SPOT") {
            return std::nullopt;
        }
    } else if (market == MarketType::Perp) {
        if (wire.inst_type != "SWAP") {
            return std::nullopt;
        }
    } else {
        return std::nullopt; // futures and options need expiry/strike first
    }

    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.lot_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.inst_id);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Okx, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (market == MarketType::Spot) {
        base.contract_value = *parse_decimal_e9("1");
        base.kind = ContractKind::Linear;
        return SpotInstrument{base};
    }
    const auto ct_val = parse_decimal_e9(wire.ct_val);
    const auto ct_mult = whole_number(wire.ct_mult);
    if (!ct_val.has_value() || ct_val->raw <= 0 || !ct_mult.has_value()) {
        return std::nullopt;
    }
    // BTC-USDT-SWAP and BTC-USD-SWAP differ ONLY here: ctVal is BTC on the
    // first and USD on the second, and the numbers look equally plausible.
    // A row that will not say which is refused rather than assumed linear.
    if (wire.ct_type == "linear") {
        base.kind = ContractKind::Linear;
    } else if (wire.ct_type == "inverse") {
        base.kind = ContractKind::Inverse;
    } else {
        return std::nullopt;
    }
    base.contract_value = ct_val->scaled_by(*ct_mult);
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::okx
