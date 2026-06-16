#include <raikiri/common/int128.hpp>
#include <raikiri/venues/kraken/map.hpp>

#include <limits>
#include <optional>

namespace raikiri::venues::kraken {

namespace {

// Guards the int128 accumulator and the power-of-ten loops below. No live
// Kraken token comes close: the longest observed mantissa is 17 digits and
// the largest exponent shift is 10.
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

// Splits a JSON numeric token into an exact mantissa and a base-10 exponent.
// Kraken spells one field both ways, so reading only plain decimals silently
// loses whichever frames took the other form.
std::optional<Decomposed> decompose(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    Decomposed out{};
    std::size_t i = 0;
    if (text[i] == '+' || text[i] == '-') {
        out.negative = text[i] == '-';
        ++i;
    }
    int digits = 0;
    int fractional = 0;
    bool seen_digit = false;
    bool seen_dot = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.') {
            if (seen_dot) {
                return std::nullopt;
            }
            seen_dot = true;
            continue;
        }
        if (c == 'e' || c == 'E') {
            break;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        seen_digit = true;
        if (out.mantissa != 0 || c != '0') {
            if (++digits > k_max_mantissa_digits) {
                return std::nullopt;
            }
            out.mantissa = out.mantissa * 10 + (c - '0');
        }
        if (seen_dot) {
            ++fractional;
        }
    }
    if (!seen_digit) {
        return std::nullopt;
    }
    out.exponent = -fractional;

    if (i < text.size()) {
        ++i; // skip the 'e'
        bool exponent_negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exponent_negative = text[i] == '-';
            ++i;
        }
        if (i >= text.size()) {
            return std::nullopt;
        }
        int value = 0;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
            if (value > k_max_shift * 4) {
                return std::nullopt;
            }
        }
        out.exponent += exponent_negative ? -value : value;
    }
    return out;
}

// mantissa * 10^exponent -> 1e9 fixed point. `exact` decides what happens to
// significance below 1e-9: refuse it, or truncate toward zero.
std::optional<FixedE9> scale_to_e9(const Decomposed& parts, bool exact) noexcept {
    int shift = parts.exponent + 9;
    if (shift > k_max_shift || shift < -k_max_shift) {
        return std::nullopt;
    }
    int128_t value = parts.mantissa;
    for (; shift > 0; --shift) {
        value *= 10;
    }
    for (; shift < 0; ++shift) {
        if (exact && value % 10 != 0) {
            return std::nullopt;
        }
        value /= 10; // int128 division truncates toward zero, which is what we want
    }
    if (parts.negative) {
        value = -value;
    }
    constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
    if (value > k_i64_max || value < -k_i64_max) {
        return std::nullopt;
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(value));
}

// Rates, fees and turnover, never prices or sizes. All three are
// full-precision figures Kraken prints with more digits than 1e9 holds —
// relative_funding_rate 1.477885e-5 is eleven decimals, a maker rebate
// -0.00009142857 BTC is eleven more — so an exact read would refuse every
// funding frame the venue has ever sent and report every fill as free.
// Truncating here discards less than the settlement currency's own smallest
// unit (1e-9 BTC is one satoshi) and nothing divides an order by any of these
// numbers; prices and quantities stay exact.
std::optional<FixedE9> to_truncated_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means Kraken changed an instrument's precision, so the
// message is dropped rather than rounded — rounding is how an order ends up
// one tick away from the intended price.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(text);
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

// An absent numeric field means "none of it", which for a filled or remaining
// quantity is zero. An unreadable one does NOT: that is a schema event.
std::optional<QtyLots> to_lots_or_zero(std::string_view text, const InstrumentBase& base) {
    return text.empty() ? std::optional{QtyLots::from_raw(0)} : to_lots(text, base);
}

// Our own ULID coming back from the venue. A value that does not fit the field
// cannot be one of ours, so the event stays unattributed rather than being
// matched to a truncated lookalike.
bool copy_client_id(std::string_view text, ExchangeClientOrderId& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = ExchangeClientOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// `fillType` names nine things, and only two of them are a side of the book.
// A liquidation or an assignment was not our quote and not our take, so it is
// left Unknown rather than filed under the tier it did not earn.
Liquidity liquidity_of(std::string_view fill_type) noexcept {
    if (fill_type == "maker") {
        return Liquidity::Maker;
    }
    if (fill_type == "taker" || fill_type == "takerAfterEdit") {
        return Liquidity::Taker;
    }
    return Liquidity::Unknown;
}

bool copy_order_id(std::string_view text, ExchangeOrderId& out) {
    if (text.empty()) {
        return true; // absent is fine; wrong is not
    }
    const auto id = ExchangeOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/true) : std::nullopt;
}

std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept {
    if (precision > 9 || precision < -9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::int64_t i = 0; i < precision; ++i) {
        raw /= 10;
    }
    for (std::int64_t i = precision; i < 0; ++i) {
        raw *= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.suspended) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.bid, base);
    const auto ask = to_ticks(wire.ask, base);
    const auto bid_qty = to_lots(wire.bid_size, base);
    const auto ask_qty = to_lots(wire.ask_size, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }
    if (bid->raw <= 0 || ask->raw <= 0 || bid_qty->raw <= 0 || ask_qty->raw <= 0) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    if (!price.has_value() || !qty.has_value() || price->raw <= 0 || qty->raw <= 0) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    // A liquidation print is real volume at a real price and is kept.
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_truncated_e9(wire.relative_funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // ticker_lite quotes none: not the same as zero
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto predicted = to_truncated_e9(wire.relative_funding_rate_prediction);
        predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_rate_time_ms, UnixNanos{0});
    out.interval_s = k_funding_interval_s;
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.volume.empty() && wire.volume_quote.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    TickerStats out{};
    out.instrument = base.id;
    // `volume` is the base asset on PF_ and CONTRACTS OF QUOTE on PI_, where
    // Kraken repeats it in volumeQuote unchanged. The base column is left at
    // zero on the inverse family rather than filled from a number measured in
    // dollars — Kraken states no base figure for those contracts at all.
    out.volume_24h_base = base.kind == ContractKind::Inverse
                              ? FixedE9::from_raw(0)
                              : to_truncated_e9(wire.volume).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = parse_turnover_e6(wire.volume_quote).value_or(FixedE6::from_raw(0));
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (!wire.tradeable) {
        return std::nullopt;
    }
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (!wire.last_trading_time.empty()) {
        return std::nullopt;
    }
    const bool linear = wire.symbol.starts_with("PF_");
    const bool inverse = wire.symbol.starts_with("PI_");
    if (!linear && !inverse) {
        return std::nullopt;
    }
    if (!wire.has_precision) {
        return std::nullopt; // an absent exponent is not a precision of zero
    }

    const auto tick = parse_number_e9(wire.tick_size);
    const auto lot = step_from_precision(wire.contract_value_trade_precision);
    const auto contract = parse_number_e9(wire.contract_size);
    if (!tick.has_value() || tick->raw <= 0 || !lot.has_value() || !contract.has_value() ||
        contract->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Kraken, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *contract;
    // PF_ settles in the quote currency, PI_ in the base one. The prefix is the
    // only thing that says so — Kraken's row carries no explicit flag.
    base.kind = linear ? ContractKind::Linear : ContractKind::Inverse;
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOpenOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto filled = to_lots_or_zero(wire.filled_size, base);
    const auto unfilled = to_lots_or_zero(wire.unfilled_size, base);
    if (!filled.has_value() || !unfilled.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = wire.buy ? Side::Buy : Side::Sell;
    if (!copy_client_id(wire.cli_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    // `untouched` and `partiallyFilled` are the only two states an order can be
    // in and still appear here; anything else is a shape we do not understand.
    if (wire.status == "untouched") {
        out.status = OrderStatus::Accepted;
    } else if (wire.status == "partiallyFilled") {
        out.status = OrderStatus::PartiallyFilled;
    } else {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.limit_price, base); price.has_value()) {
        out.price = *price;
    }
    out.qty = QtyLots::from_raw(filled->raw + unfilled->raw);
    out.leaves = *unfilled;
    out.ts_venue = ms_to_unix(wire.last_update_ms, UnixNanos{0});
    return out;
}

std::optional<exec::FillReport> map_rest_fill(const WireFill& wire,
                                              const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.size, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    exec::FillReport out{};
    out.instrument = base.id;
    out.side = wire.buy ? Side::Buy : Side::Sell;
    if (!copy_client_id(wire.cli_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (!wire.fill_id.empty()) {
        const auto exec_id = ExecId::from(wire.fill_id);
        if (!exec_id.has_value()) {
            return std::nullopt; // a dedupe key we cannot store is not usable
        }
        out.exec_id = *exec_id;
    }
    out.price = *price;
    out.qty = *qty;
    out.liquidity = liquidity_of(wire.fill_type);
    // The pull carries no fee: /fills reports realized PnL, never commission.
    out.ts_venue = ms_to_unix(wire.fill_ms, UnixNanos{0});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto size = to_lots(wire.size, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = wire.is_long ? *size : QtyLots::from_raw(-size->raw);
    if (const auto avg = to_ticks(wire.price, base); avg.has_value()) {
        out.avg_entry = *avg;
    }
    // The row carries no update time of its own; the envelope's serverTime
    // describes the read, not the position, and dating one with the other is
    // how a stale reconciliation looks fresh.
    return out;
}

DeltaEffect effect_of(std::string_view reason, bool is_cancel) noexcept {
    // A fill is reported by the fills feed, which carries the price, the size
    // and the exec id this frame does not. Emitting anything here would either
    // duplicate it or contradict it.
    if (reason == "full_fill" || reason == "partial_fill") {
        return DeltaEffect::Silent;
    }
    if (is_cancel) {
        // Kraken spells two of these as sentences rather than as names, and a
        // near-miss is silently a different verdict: the order was refused, so
        // the OMS must hear Rejected and stop waiting for a fill.
        if (reason == "not_enough_margin" ||
            reason == "post_order_failed_because_it_would_filled" ||
            reason == "ioc_order_failed_because_it_would_not_be_executed" ||
            reason == "would_execute_self" || reason == "would_not_reduce_position") {
            return DeltaEffect::Rejected;
        }
        if (reason == "order_for_edit_not_found") {
            // A failed EDIT, not a departure: the editorder response already
            // reports it, and re-reporting it as a cancel would close an order
            // that is still working.
            return DeltaEffect::Silent;
        }
        // Everything else that leaves the book — by the user, by an admin, by
        // the dead-man switch, by expiry, by liquidation — is a cancel.
        return DeltaEffect::Canceled;
    }
    if (reason == "stop_order_triggered") {
        return DeltaEffect::Triggered;
    }
    if (reason == "new_placed_order_by_user" || reason == "limit_order_from_stop") {
        return DeltaEffect::Accepted;
    }
    return DeltaEffect::Silent;
}

std::optional<OrderEvent> map_ws_order(const WireWsOrder& wire,
                                       OrderEventType type,
                                       const exec::InstrumentTable& table,
                                       UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto remaining = to_lots_or_zero(wire.remaining_qty, base);
    const auto filled = to_lots_or_zero(wire.filled_qty, base);
    if (!remaining.has_value() || !filled.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = wire.buy ? Side::Buy : Side::Sell;
    out.order_type = wire.type == "limit" ? OrderType::Limit : OrderType::Market;
    if (wire.reduce_only) {
        out.flags = OrderFlags::ReduceOnly;
    }
    if (!copy_client_id(wire.cli_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.limit_price, base); price.has_value()) {
        out.price = *price;
    }
    // `qty` on this feed is what REMAINS; the original is that plus `filled`.
    out.qty = QtyLots::from_raw(remaining->raw + filled->raw);
    out.leaves_qty = *remaining;
    out.ts_event =
        ms_to_unix(wire.last_update_ms != 0 ? wire.last_update_ms : wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent>
map_ws_fill(const WireWsFill& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    const auto remaining = to_lots_or_zero(wire.remaining_order_qty, base);
    if (!price.has_value() || !qty.has_value() || !remaining.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = wire.buy ? Side::Buy : Side::Sell;
    out.liquidity = liquidity_of(wire.fill_type);
    if (!copy_client_id(wire.cli_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (!wire.fill_id.empty()) {
        const auto exec_id = ExecId::from(wire.fill_id);
        if (!exec_id.has_value()) {
            return std::nullopt;
        }
        out.exec_id = *exec_id;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = *remaining;
    // `fee_paid` is denominated in the instrument's SETTLEMENT currency — base
    // on the inverse PI_ families, quote on the linear PF_ ones — which
    // portfolio::settle_side(base.kind) recovers from the id this event already
    // carries. It travels verbatim rather than through a conversion this frame
    // has no rate for.
    if (const auto fee = to_truncated_e9(wire.fee_paid); fee.has_value()) {
        out.fee = *fee;
    }
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::kraken
