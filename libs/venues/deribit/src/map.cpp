#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/venues/deribit/map.hpp>

#include <limits>
#include <optional>

namespace raikiri::venues::deribit {

namespace {

// The rate's UNIT, not a payment cadence — see map_funding.
constexpr std::uint32_t k_funding_basis_s = 8 * 60 * 60;

// UnixNanos wraps past ~2554-07-21 in the millisecond multiply, and Deribit
// parks perpetuals at 32503708800000 (year 3000) as an expiry sentinel.
constexpr std::uint64_t k_max_expiry_ms = std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL;

// Real Deribit exponents live in [-5, 5]; this only bounds the shift below.
constexpr int k_max_exponent = 60;

[[nodiscard]] constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr std::optional<uint128_t> pow10(int n) noexcept {
    if (n < 0 || n > 38) { // 10^38 is the last that fits 128 bits
        return std::nullopt;
    }
    uint128_t out = 1;
    for (int i = 0; i < n; ++i) {
        out *= 10;
    }
    return out;
}

constexpr uint128_t k_mantissa_cap = *pow10(30);

} // namespace

// The value is mantissa * 10^(exponent - fraction_digits), so its e9 form is
// that mantissa shifted by 9 + exponent - fraction_digits: a multiply when the
// shift is positive, an EXACT division when negative. Failing to divide evenly
// means more precision than FixedE9 models, which is refused, not rounded.
std::optional<FixedE9> parse_number_e9(std::string_view token) noexcept {
    if (token.empty()) {
        return std::nullopt;
    }
    std::size_t i = 0;
    bool negative = false;
    if (token[i] == '+' || token[i] == '-') {
        negative = token[i] == '-';
        ++i;
    }

    uint128_t mantissa = 0;
    int fraction_digits = 0;
    bool seen_point = false;
    bool seen_digit = false;
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (c == '.') {
            if (seen_point) {
                return std::nullopt;
            }
            seen_point = true;
            continue;
        }
        if (c == 'e' || c == 'E') {
            break;
        }
        if (!is_digit(c)) {
            return std::nullopt;
        }
        seen_digit = true;
        mantissa = (mantissa * 10) + static_cast<unsigned>(c - '0');
        if (mantissa > k_mantissa_cap) {
            return std::nullopt;
        }
        if (seen_point) {
            ++fraction_digits;
        }
    }
    if (!seen_digit) {
        return std::nullopt;
    }

    int exponent = 0;
    if (i < token.size()) {
        ++i; // the 'e'
        bool exponent_negative = false;
        if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
            exponent_negative = token[i] == '-';
            ++i;
        }
        bool seen_exponent_digit = false;
        for (; i < token.size(); ++i) {
            if (!is_digit(token[i])) {
                return std::nullopt;
            }
            seen_exponent_digit = true;
            exponent = (exponent * 10) + (token[i] - '0');
            if (exponent > k_max_exponent) {
                return std::nullopt;
            }
        }
        if (!seen_exponent_digit) {
            return std::nullopt;
        }
        if (exponent_negative) {
            exponent = -exponent;
        }
    }

    const int shift = 9 + exponent - fraction_digits;
    uint128_t scaled = 0;
    if (shift >= 0) {
        const auto factor = pow10(shift);
        if (!factor.has_value()) {
            return std::nullopt;
        }
        scaled = mantissa * *factor;
    } else {
        const auto divisor = pow10(-shift);
        if (!divisor.has_value() || mantissa % *divisor != 0) {
            return std::nullopt;
        }
        scaled = mantissa / *divisor;
    }
    if (scaled > static_cast<uint128_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    const auto raw = static_cast<std::int64_t>(scaled);
    return FixedE9::from_raw(negative ? -raw : raw);
}

namespace {

// Exact conversion against the instrument's own step. Failing to divide evenly
// means Deribit changed an instrument's precision, so the value is dropped:
// rounding is how a reference price lands a tick off what actually printed.
std::optional<std::int64_t> exact_steps(std::string_view token, FixedE9 step) {
    if (token.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(token);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

std::optional<PriceTicks> to_ticks(std::string_view token, const InstrumentBase& base) {
    const auto steps = exact_steps(token, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view token, const InstrumentBase& base) {
    const auto steps = exact_steps(token, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 || ms > k_max_expiry_ms ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
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
    if (bid->raw <= 0 || ask->raw <= 0) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    // The one timestamp is the matching-engine one; there is no send time.
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
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
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    if (wire.current_funding.empty()) {
        return std::nullopt;
    }
    const auto rate = parse_number_e9(wire.current_funding);
    if (!rate.has_value()) {
        return std::nullopt;
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    out.interval_s = k_funding_basis_s;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}


namespace {

// Deribit's own vocabulary for where an order stands.
enum class OrderState : std::uint8_t { Open, Filled, Rejected, Cancelled, Triggered, Untriggered };

std::optional<OrderState> state_of(std::string_view text) noexcept {
    if (text == "open") {
        return OrderState::Open;
    }
    if (text == "filled") {
        return OrderState::Filled;
    }
    if (text == "rejected") {
        return OrderState::Rejected;
    }
    if (text == "cancelled") {
        return OrderState::Cancelled;
    }
    if (text == "triggered") {
        return OrderState::Triggered;
    }
    if (text == "untriggered") {
        return OrderState::Untriggered;
    }
    return std::nullopt;
}

// A remainder Deribit killed because the order said so, versus one a human or
// the venue withdrew. Deribit has no "expired" state, so the time-in-force is
// the only witness, and the difference is what an operator reads to tell an
// IOC working as designed from a venue cancelling underneath a strategy.
bool ends_by_expiry(std::string_view tif) noexcept {
    return tif == "immediate_or_cancel" || tif == "fill_or_kill";
}

// Both terms ride the same unit, so their difference is a lot count. Anything
// that does not divide exactly means the instrument's step moved under us.
std::optional<QtyLots> leaves_of(const WireOrder& wire, const InstrumentBase& base) {
    const auto amount = to_lots(wire.amount, base);
    if (!amount.has_value()) {
        return std::nullopt;
    }
    if (wire.filled_amount.empty()) {
        return amount;
    }
    const auto filled = to_lots(wire.filled_amount, base);
    if (!filled.has_value() || filled->raw > amount->raw) {
        return std::nullopt;
    }
    return QtyLots::from_raw(amount->raw - filled->raw);
}

void stamp(OrderEvent& event,
           const WireOrder& wire,
           const InstrumentBase& base,
           UnixNanos ts_init) {
    event.side = wire.sell ? Side::Sell : Side::Buy;
    event.instrument = base.id;
    if (const auto id = ExchangeClientOrderId::from(wire.label); id.has_value()) {
        event.exchange_client_order_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        event.exchange_order_id = *id;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        event.price = *price;
    }
    if (const auto qty = to_lots(wire.amount, base); qty.has_value()) {
        event.qty = *qty;
    }
    event.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    event.ts_init = ts_init;
}

} // namespace

std::optional<QtyLots> order_leaves(const WireOrder& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    return leaves_of(wire, base_of(*instrument));
}

std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt; // another market or another currency: not ours
    }
    const auto state = state_of(wire.state);
    if (!state.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto leaves = leaves_of(wire, base);
    if (!leaves.has_value()) {
        return std::nullopt;
    }

    OrderEvent event{};
    switch (*state) {
    case OrderState::Open: {
        const auto qty = to_lots(wire.amount, base);
        if (!qty.has_value()) {
            return std::nullopt;
        }
        // A partially filled order is already PartiallyFilled to the FSM, and
        // Accepted is not a transition out of it. The Filled events the same
        // frame carries say everything this row would repeat.
        if (leaves->raw != qty->raw) {
            return std::nullopt;
        }
        event.type = OrderEventType::Accepted;
        break;
    }
    case OrderState::Filled:
        // Every unit of a filled order is accounted for by its trades, which
        // arrive on their own channel. Synthesising a Filled here would book
        // the quantity twice.
        return std::nullopt;
    case OrderState::Rejected:
        event.type = OrderEventType::Rejected;
        break;
    case OrderState::Cancelled:
        event.type = ends_by_expiry(wire.tif) ? OrderEventType::Expired
                                              : OrderEventType::Canceled;
        break;
    case OrderState::Triggered:
        event.type = OrderEventType::Triggered;
        break;
    case OrderState::Untriggered:
        // A resting trigger order. This adapter places none, so one appearing
        // was placed elsewhere and there is no local order to move.
        return std::nullopt;
    }
    stamp(event, wire, base, ts_init);
    // A rejected order never rested, so nothing is left of it. Every other
    // state here reports what the venue itself says went unfilled.
    event.leaves_qty =
        event.type == OrderEventType::Rejected ? QtyLots::from_raw(0) : *leaves;
    return event;
}

std::optional<OrderEvent> map_fill(const WireUserTrade& wire,
                                   const exec::InstrumentTable& table,
                                   QtyLots leaves,
                                   UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.amount, base);
    if (!price.has_value() || !qty.has_value() || qty->raw <= 0) {
        return std::nullopt;
    }

    if (leaves.is_undef()) {
        // The OMS reads leaves_qty as authoritative on a fill and subtracts it
        // from the order quantity; an undefined one is not a number that
        // arithmetic survives, so the caller must resolve it first.
        return std::nullopt;
    }

    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.side = wire.sell ? Side::Sell : Side::Buy;
    event.instrument = base.id;
    if (const auto id = ExchangeClientOrderId::from(wire.label); id.has_value()) {
        event.exchange_client_order_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        event.exchange_order_id = *id;
    }
    if (const auto id = ExecId::from(wire.trade_id); id.has_value()) {
        event.exec_id = *id;
    }
    event.price = *price;
    event.qty = *qty;
    event.leaves_qty = leaves;
    // The fee is in fee_currency, which for a reversed future and for an
    // option alike is the base coin — the same currency the position settles
    // in, never the USD the future is priced in.
    if (const auto fee = parse_number_e9(wire.fee); fee.has_value()) {
        event.fee = *fee;
    }
    event.liquidity = wire.liquidity_stated
                          ? (wire.maker ? Liquidity::Maker : Liquidity::Taker)
                          : Liquidity::Unknown;
    event.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    event.ts_init = ts_init;
    return event;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto state = state_of(wire.state);
    if (!state.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = to_lots(wire.amount, base);
    const auto leaves = leaves_of(wire, base);
    if (!qty.has_value() || !leaves.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport report{};
    switch (*state) {
    case OrderState::Open:
        report.status = leaves->raw == qty->raw ? OrderStatus::Accepted
                                                : OrderStatus::PartiallyFilled;
        break;
    case OrderState::Filled:
        report.status = OrderStatus::Filled;
        break;
    case OrderState::Rejected:
        report.status = OrderStatus::Rejected;
        break;
    case OrderState::Cancelled:
        report.status = ends_by_expiry(wire.tif) ? OrderStatus::Expired : OrderStatus::Canceled;
        break;
    case OrderState::Triggered:
        report.status = OrderStatus::Triggered;
        break;
    case OrderState::Untriggered:
        // Armed but not yet live. The domain has no separate status for it and
        // Accepted is what it behaves as: resting at the venue, not filled.
        report.status = OrderStatus::Accepted;
        break;
    }
    if (const auto id = ExchangeClientOrderId::from(wire.label); id.has_value()) {
        report.exchange_client_order_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        report.exchange_order_id = *id;
    }
    report.instrument = base.id;
    report.side = wire.sell ? Side::Sell : Side::Buy;
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        report.price = *price;
    }
    report.qty = *qty;
    report.leaves = *leaves;
    report.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{0});
    return report;
}

std::optional<exec::FillReport> map_fill_report(const WireUserTrade& wire,
                                                const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.amount, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    exec::FillReport report{};
    if (const auto id = ExecId::from(wire.trade_id); id.has_value()) {
        report.exec_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        report.exchange_order_id = *id;
    }
    if (const auto id = ExchangeClientOrderId::from(wire.label); id.has_value()) {
        report.exchange_client_order_id = *id;
    }
    report.instrument = base.id;
    report.side = wire.sell ? Side::Sell : Side::Buy;
    report.price = *price;
    report.qty = *qty;
    if (const auto fee = parse_number_e9(wire.fee); fee.has_value()) {
        report.fee = *fee;
    }
    report.liquidity = wire.liquidity_stated
                           ? (wire.maker ? Liquidity::Maker : Liquidity::Taker)
                           : Liquidity::Unknown;
    report.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{0});
    return report;
}

std::optional<exec::PositionReport>
map_position(const WirePosition& wire, const exec::InstrumentTable& table, UnixNanos ts_venue) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    // Signed: the sign IS the direction, and exact_steps keeps it because a
    // negative multiple of the step still divides evenly.
    const auto size = to_lots(wire.size, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport report{};
    report.instrument = base.id;
    report.net_qty = *size;
    if (const auto entry = to_ticks(wire.average_price, base); entry.has_value()) {
        report.avg_entry = *entry;
    }
    report.ts_venue = ts_venue;
    return report;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (!wire.is_active) {
        return std::nullopt; // delivered, expired or not yet listed
    }
    const bool perpetual = wire.settlement_period == "perpetual";
    switch (market) {
    case MarketType::Perp:
        if (wire.kind != "future" || !perpetual) {
            return std::nullopt;
        }
        break;
    case MarketType::Future:
        // Both share kind "future", and confusing them is a position bug.
        if (wire.kind != "future" || perpetual) {
            return std::nullopt;
        }
        break;
    case MarketType::Option:
        if (wire.kind != "option") {
            return std::nullopt;
        }
        break;
    case MarketType::Spot:
        return std::nullopt; // listed, but the caps row calls it vestigial
    }
    if (wire.instrument_type != "reversed") {
        return std::nullopt;
    }

    const auto tick = parse_number_e9(wire.tick_size);
    const auto lot = parse_number_e9(wire.min_trade_amount);
    const auto contract = parse_number_e9(wire.contract_size);
    if (!tick.has_value() || !lot.has_value() || !contract.has_value() || tick->raw <= 0 ||
        lot->raw <= 0 || contract->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.instrument_name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Deribit, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    // The smallest order the venue will take, in the same unit as lot_size.
    base.min_notional = *lot;
    // ONE amount unit, not one contract. portfolio::settlement_value reads
    // qty x lot_size x contract_value as the notional, and lot_size here is
    // min_trade_amount — already denominated in what Deribit's `amount` field
    // counts (USD on the reversed futures, coin on the options). Storing the
    // venue's contract_size beside it would multiply the notional by that
    // block a second time: a BTC-PERPETUAL lot would price at 100 USD instead
    // of 10, and every PnL derived from it would be ten times the truth.
    // contract_size itself survives as lot_size wherever the two agree, which
    // on every reversed future they do.
    base.contract_value = FixedE9::from_raw(k_e9);
    // Deribit states the direction itself and the guard above already refused
    // anything but "reversed", so every future reaching here is the venue's own
    // word for inverse: the amount is USD, settlement is in the base coin.
    base.kind = ContractKind::Inverse;

    if (market == MarketType::Option) {
        // A BTC option quotes its PREMIUM in BTC and sizes itself in BTC, so
        // its notional is amount x price with no reciprocal anywhere —
        // the linear formula, even though the same catalogue calls the row
        // "reversed" and the future beside it genuinely is inverse. Inheriting
        // the future's arithmetic here would price a premium by dividing.
        base.kind = ContractKind::Linear;
        const auto strike = exact_steps(wire.strike, base.tick_size);
        if (!strike.has_value() || *strike <= 0) {
            return std::nullopt;
        }
        if (wire.expiration_ms == 0 || wire.expiration_ms > k_max_expiry_ms) {
            return std::nullopt;
        }
        if (wire.option_type != "put" && wire.option_type != "call") {
            return std::nullopt;
        }
        const auto put_call = wire.option_type == "put" ? PutCall::Put : PutCall::Call;
        return OptionInstrument{base,
                                PriceTicks::from_raw(*strike),
                                ms_to_unix(wire.expiration_ms, UnixNanos{0}),
                                put_call};
    }

    if (market == MarketType::Future) {
        if (wire.expiration_ms == 0 || wire.expiration_ms > k_max_expiry_ms) {
            return std::nullopt;
        }
        return FutureInstrument{base, ms_to_unix(wire.expiration_ms, UnixNanos{0})};
    }
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::deribit
