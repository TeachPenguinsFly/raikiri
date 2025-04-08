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
    // bbo-tbt carries one timestamp, and it is the matching-engine one — OKX
    // has no separate send time to confuse it with.
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
    // mark_price stays undefined: OKX publishes mark on its own channel, and a
    // zero here would read as a real price of zero.
    //
    // fundingTime, not nextFundingTime — see wire.hpp. The pair also gives the
    // interval exactly, which /public/instruments does not carry and which is
    // not 8h on every OKX contract.
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
    // A row that is not live yet ships every numeric field as an empty string,
    // so this check has to come first or the steps below fail for the wrong
    // reason.
    if (wire.state != "live") {
        return std::nullopt;
    }
    if (market == MarketType::Spot) {
        if (wire.inst_type != "SPOT") {
            return std::nullopt;
        }
    } else if (market == MarketType::Perp) {
        // Asking for perps and getting a dated FUTURES contract back is a
        // whole category of position bug, and both share this endpoint.
        if (wire.inst_type != "SWAP") {
            return std::nullopt;
        }
    } else {
        return std::nullopt; // futures and options need expiry/strike first
    }

    // KNOWN LIMIT, not a venue quirk: FixedE9 carries nine fractional digits
    // and OKX lists SATS-USDT-SWAP at a tick of 0.000000000001. That row is
    // live and is refused here — correct under the no-rounding rule, and the
    // only way to reach it is to widen the fixed-point scale across the
    // domain. One instrument of 442 today; worth revisiting if the count of
    // sub-nano ticks grows rather than papering over it per venue.
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
    // min_notional stays undefined: OKX's floor is minSz, a count of
    // CONTRACTS, and converting it to a notional needs a price we do not have
    // here. A zero would read as "no minimum", which is the wrong answer.
    if (market == MarketType::Spot) {
        base.contract_value = *parse_decimal_e9("1");
        return SpotInstrument{base};
    }
    // A SWAP quotes in contracts: one contract is ctVal * ctMult of ctValCcy
    // (0.01 BTC on BTC-USDT-SWAP, 100 USD on the inverse BTC-USD-SWAP).
    // Binance's USD-M perps quote in base units and can pin this to 1; OKX
    // cannot, and a hardcoded 1 would misprice every notional by ~100x.
    const auto ct_val = parse_decimal_e9(wire.ct_val);
    const auto ct_mult = whole_number(wire.ct_mult);
    if (!ct_val.has_value() || ct_val->raw <= 0 || !ct_mult.has_value()) {
        return std::nullopt;
    }
    base.contract_value = ct_val->scaled_by(*ct_mult);
    // funding_interval_s is left at zero on purpose: the catalogue endpoint
    // does not carry it, OKX runs both 8h and 4h contracts, and the funding
    // frame states the real boundaries. Guessing 8h here would be wrong for a
    // growing set of instruments and invisible when it is.
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::okx
