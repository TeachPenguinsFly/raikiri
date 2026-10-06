#include <raikiri/common/int128.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/spot.hpp>

#include <limits>
#include <optional>

namespace raikiri::venues::kraken {

namespace {

// bounds the int128 accumulator and power-of-ten loops
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

// splits a numeric token into exact mantissa and base-10 exponent
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
        ++i;
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

// exact refuses digits below 1e-9; otherwise truncates toward zero
std::optional<int128_t> scale_to_e9_wide(const Decomposed& parts, bool exact) noexcept {
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
        value /= 10; // int128 division truncates toward zero
    }
    return parts.negative ? -value : value;
}

std::optional<FixedE9> scale_to_e9(const Decomposed& parts, bool exact) noexcept {
    const auto value = scale_to_e9_wide(parts, exact);
    constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
    if (!value.has_value() || *value > k_i64_max || *value < -k_i64_max) {
        return std::nullopt;
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(*value));
}

// rates, fees and turnover carry more digits than 1e9; truncated here, prices and sizes stay exact
std::optional<FixedE9> to_truncated_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

// volume times vwap in quote units; a meme coin's base volume overflows e9, so the product is taken exactly
std::optional<FixedE6> notional_e6(std::string_view qty, std::string_view price) noexcept {
    const auto a = decompose(qty);
    const auto b = decompose(price);
    if (!a.has_value() || !b.has_value() || a->negative || b->negative) {
        return std::nullopt;
    }
    constexpr int128_t k_bound = int128_t{1} << 100;
    if (a->mantissa != 0 && b->mantissa > k_bound / a->mantissa) {
        return std::nullopt;
    }
    int128_t value = a->mantissa * b->mantissa;
    int shift = a->exponent + b->exponent + 6;
    if (shift > k_max_shift || shift < -k_max_mantissa_digits * 2) {
        return std::nullopt;
    }
    constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
    for (; shift > 0 && value <= k_i64_max; --shift) {
        value *= 10;
    }
    for (; shift < 0; ++shift) {
        value /= 10;
    }
    if (value > k_i64_max) {
        return std::nullopt;
    }
    return FixedE6::from_raw(static_cast<std::int64_t>(value));
}

std::optional<PriceTicks> to_ticks(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

// absent means zero; an unreadable value is a schema error
std::optional<QtyLots> to_lots_or_zero(std::string_view text, const InstrumentBase& base) {
    return text.empty() ? std::optional{QtyLots::from_raw(0)} : to_lots(text, base);
}

// a value too long for the field cannot be ours, so the event stays unattributed
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

// only maker and taker are book sides; liquidation and assignment stay Unknown
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

std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) noexcept {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    const auto value = parts.has_value() ? scale_to_e9_wide(*parts, /*exact=*/true) : std::nullopt;
    if (!value.has_value() || *value % step.raw != 0) {
        return std::nullopt;
    }
    const auto steps = *value / step.raw;
    constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
    if (steps > k_i64_max || steps < -k_i64_max) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(steps);
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

std::optional<Bbo> map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ms_to_unix(wire.time_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    // liquidation prints are kept as real volume
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.time_ms, UnixNanos{});
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
    if (const auto predicted = to_truncated_e9(wire.relative_funding_rate_prediction); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    // both sit off the tick grid, so they are truncated onto it rather than refused
    if (const auto mark = to_truncated_e9(wire.mark_price); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    if (const auto index = to_truncated_e9(wire.index); index.has_value() && base.tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(index->raw / base.tick_size.raw);
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_rate_time_ms, UnixNanos{0});
    out.interval_s = k_funding_interval_s;
    out.ts_event = ms_to_unix(wire.time_ms, UnixNanos{});
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
    // base stays zero on PI_, where volume counts contracts
    out.volume_24h_base = base.kind == ContractKind::Inverse
                              ? FixedE9::from_raw(0)
                              : to_truncated_e9(wire.volume).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = parse_turnover_e6(wire.volume_quote).value_or(FixedE6::from_raw(0));
    // counted in contracts: base units on PF_, dollars on PI_, which the mark turns into coins
    if (const auto contracts = to_truncated_e9(wire.open_interest); contracts.has_value()) {
        const auto held = turnover_e9(*contracts, base.contract_value);
        const auto price = to_truncated_e9(wire.mark_price.empty() ? wire.last : wire.mark_price);
        if (base.kind != ContractKind::Inverse) {
            out.open_interest = held;
        } else if (!held.is_undef() && price.has_value() && price->raw > 0) {
            const int128_t coins = static_cast<int128_t>(held.raw) * k_e9 / price->raw;
            if (coins <= std::numeric_limits<std::int64_t>::max()) {
                out.open_interest = FixedE9::from_raw(static_cast<std::int64_t>(coins));
            }
        }
    }
    out.ts_event = ms_to_unix(wire.time_ms, UnixNanos{});
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
    if (!tick.has_value() || tick->raw <= 0 || !lot.has_value() || !contract.has_value() || contract->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional; InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Kraken, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    // the pair the spot side names too; a row without one keeps the venue's symbol
    base.local_symbol = canonical_symbol(wire.base, wire.quote).value_or(*symbol);
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *contract;
    // PF_ settles in quote and PI_ in base; only the prefix says so
    base.kind = linear ? ContractKind::Linear : ContractKind::Inverse;
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table) {
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
    // only untouched and partiallyFilled orders appear here
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

std::optional<exec::FillReport> map_rest_fill(const WireFill& wire, const exec::InstrumentTable& table) {
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
    // /fills carries no fee, only realized pnl
    out.ts_venue = ms_to_unix(wire.fill_ms, UnixNanos{0});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table) {
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
    // the row has no update time; serverTime dates the read, not the position
    return out;
}

DeltaEffect effect_of(std::string_view reason, bool is_cancel) noexcept {
    // the fills feed reports fills with price, size and exec id
    if (reason == "full_fill" || reason == "partial_fill") {
        return DeltaEffect::Silent;
    }
    if (is_cancel) {
        // kraken spells two reasons as sentences; match them exactly
        if (reason == "not_enough_margin" || reason == "post_order_failed_because_it_would_filled" ||
            reason == "ioc_order_failed_because_it_would_not_be_executed" || reason == "would_execute_self" ||
            reason == "would_not_reduce_position") {
            return DeltaEffect::Rejected;
        }
        if (reason == "order_for_edit_not_found") {
            // a failed edit; the editorder response already reports it
            return DeltaEffect::Silent;
        }
        // anything else that leaves the book is a cancel
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

std::optional<OrderEvent>
map_ws_order(const WireWsOrder& wire, OrderEventType type, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    // qty here is what remains; the original adds filled
    out.qty = QtyLots::from_raw(remaining->raw + filled->raw);
    out.leaves_qty = *remaining;
    out.ts_event = ms_to_unix(wire.last_update_ms != 0 ? wire.last_update_ms : wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_ws_fill(const WireWsFill& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    // fee_paid is in the settlement currency and travels unconverted
    if (const auto fee = to_truncated_e9(wire.fee_paid); fee.has_value()) {
        out.fee = *fee;
    }
    out.ts_event = ms_to_unix(wire.time_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_spot_pair(const WireSpotPair& wire) {
    if (wire.status != "online") {
        return std::nullopt; // cancel_only, post_only, limit_only, reduce_only
    }
    const auto tick = step_from_precision(wire.pair_decimals);
    const auto lot = step_from_precision(wire.lot_decimals);
    if (wire.pair_decimals < 0 || wire.lot_decimals < 0 || !tick.has_value() || !lot.has_value()) {
        return std::nullopt; // ten price decimals do not fit e9
    }
    const auto symbol = v2_symbol(wire.wsname);
    if (!symbol.has_value()) {
        return std::nullopt;
    }
    const auto view = symbol->view();
    const auto slash = view.find('/');
    const auto canonical = canonical_symbol(view.substr(0, slash), view.substr(slash + 1));
    if (!canonical.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Kraken, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = to_truncated_e9(wire.costmin).value_or(FixedE9::from_raw(0));
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = AssetClass::Crypto;
    return SpotInstrument{base};
}

std::optional<Bbo> map_spot_bbo(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.bid, base);
    const auto ask = to_ticks(wire.ask, base);
    const auto bid_qty = to_lots(wire.bid_qty, base);
    const auto ask_qty = to_lots(wire.ask_qty, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value() || bid->raw <= 0 ||
        ask->raw <= 0 || bid_qty->raw <= 0 || ask_qty->raw <= 0) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_spot_ticker_stats(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.volume.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }

    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = to_truncated_e9(wire.volume).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = notional_e6(wire.volume, wire.vwap).value_or(FixedE6::from_raw(0));
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_spot_trade(const WireSpotTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
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
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::kraken
