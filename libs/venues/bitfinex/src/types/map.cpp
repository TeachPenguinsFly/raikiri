#include <raikiri/common/int128.hpp>
#include <raikiri/venues/bitfinex/catalogue.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/market_data.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/positions.hpp>

#include <limits>
#include <optional>
#include <string>

namespace raikiri::venues::bitfinex {

namespace {

// guards the int128 accumulator; live mantissas stay well inside
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

constexpr std::string_view k_symbol_prefix = "t";

// paper-trading pairs share the production /conf endpoint under a "TEST" prefix
constexpr std::string_view k_paper_prefix = "TEST";

// futures pairs carry "F0" on both legs; no spot pair contains it
constexpr std::string_view k_futures_marker = "F0";

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

// bitfinex sends the same field as plain decimal or scientific notation
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

// exact refuses digits below 1e-9; otherwise they truncate toward zero
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
        value /= 10; // truncates toward zero
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

// values off the 1e-8 grid are dropped unrounded
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

// rates stay at 1e9 scale; truncation below 1e-9 is harmless for them
std::optional<FixedE9> to_rate_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

// an amount, so it truncates below 1e-9 rather than refusing
std::optional<FixedE9> to_amount_e9(std::string_view text) noexcept {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

// both amounts are signed; the sign never reaches QtyLots
struct SignedToken {
    std::string_view magnitude;
    bool negative{false};
};

SignedToken split_sign(std::string_view text) noexcept {
    SignedToken out{.magnitude = text, .negative = false};
    if (!out.magnitude.empty() && (out.magnitude.front() == '-' || out.magnitude.front() == '+')) {
        out.negative = out.magnitude.front() == '-';
        out.magnitude.remove_prefix(1);
    }
    return out;
}

// oversize ids are refused; a truncated id names another order
bool copy_id(std::string_view text, ExchangeOrderId& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = ExchangeOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

bool copy_exec_id(std::string_view text, ExecId& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = ExecId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// fees truncate at 1e-9; bitfinex signs a charge negative, so it is flipped
FixedE9 fee_e9(std::string_view text) noexcept {
    const auto value = to_rate_e9(text);
    return value.has_value() ? FixedE9::from_raw(-value->raw) : FixedE9::from_raw(0);
}

const InstrumentBase* base_for(std::string_view symbol, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

// the side comes from amount_orig; amount is 0 once filled
Side side_of(std::string_view amount_orig) noexcept {
    return split_sign(amount_orig).negative ? Side::Sell : Side::Buy;
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

std::optional<Bbo> map_bbo(const WireTicker& wire,
                           std::string_view symbol,
                           const exec::InstrumentTable& table,
                           UnixNanos ts_event,
                           UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
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
    if (bid->raw <= 0 || ask->raw <= 0 || bid_qty->raw <= 0 || ask_qty->raw <= 0 ||
        bid->raw >= ask->raw) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ts_event.ns == 0 ? ts_init : ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTicker& wire,
                                            std::string_view symbol,
                                            const exec::InstrumentTable& table,
                                            UnixNanos ts_event,
                                            UnixNanos ts_init) {
    const auto volume = to_amount_e9(wire.volume);
    if (!volume.has_value()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = *volume;
    // the channel states no quote volume
    out.volume_24h_quote = FixedE6::from_raw(0);
    out.ts_event = ts_event.ns == 0 ? ts_init : ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire,
                                   std::string_view symbol,
                                   const exec::InstrumentTable& table,
                                   UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto amount = split_sign(wire.amount);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(amount.magnitude, base);
    if (!price.has_value() || !qty.has_value() || price->raw <= 0 || qty->raw <= 0) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = amount.negative ? Side::Sell : Side::Buy;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireDerivStatus& wire,
                                         std::string_view symbol,
                                         const exec::InstrumentTable& table,
                                         UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_rate_e9(wire.current_funding);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto predicted = to_rate_e9(wire.next_funding_accrued); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    out.interval_s = k_funding_interval_s;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WirePairInfo& wire, MarketType market) {
    if (wire.pair.starts_with(k_paper_prefix)) {
        return std::nullopt;
    }
    const bool futures = wire.pair.find(k_futures_marker) != std::string_view::npos;
    if (market == MarketType::Spot) {
        if (futures) {
            return std::nullopt;
        }
    } else if (market == MarketType::Perp) {
        if (!futures) {
            return std::nullopt;
        }
    } else {
        return std::nullopt; // dated futures and options are not listed here
    }
    const auto min_size = parse_number_e9(wire.min_order_size);
    if (!min_size.has_value() || min_size->raw <= 0) {
        return std::nullopt;
    }

    std::string wire_symbol;
    wire_symbol.reserve(k_symbol_prefix.size() + wire.pair.size());
    wire_symbol.append(k_symbol_prefix).append(wire.pair);
    const auto symbol = SymbolText::from(wire_symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional; InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Bitfinex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = FixedE9::from_raw(k_step_e9);
    base.lot_size = FixedE9::from_raw(k_step_e9);
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<OrderEvent> map_order_event(const WireOrder& wire,
                                          const exec::InstrumentTable& table,
                                          const ExchangeClientOrderId& client_id,
                                          UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    OrderEventType type{};
    switch (wire.status) {
    case WireOrderStatus::Active:
        type = OrderEventType::Accepted;
        break;
    case WireOrderStatus::Canceled:
        type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::Rejected:
        type = OrderEventType::Rejected;
        break;
    case WireOrderStatus::PartiallyFilled:
    case WireOrderStatus::Executed:
        // fills come from te/tu
        return std::nullopt;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }

    const auto ordered = split_sign(wire.amount_orig);
    const auto qty = to_lots(ordered.magnitude, *base);
    if (!qty.has_value() || qty->raw <= 0) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base->id;
    out.side = side_of(wire.amount_orig);
    out.order_type = wire.type.ends_with("MARKET") ? OrderType::Market : OrderType::Limit;
    if ((wire.flags & k_flag_post_only) != 0) {
        out.flags = OrderFlags::PostOnly;
    }
    out.exchange_client_order_id = client_id;
    if (!copy_id(wire.id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    // nothing rests after a cancel or rejection
    out.leaves_qty = QtyLots::from_raw(0);
    if (type == OrderEventType::Accepted) {
        const auto left = to_lots(split_sign(wire.amount).magnitude, *base);
        out.leaves_qty = left.has_value() ? *left : *qty;
    }
    out.ts_event = ms_to_unix(wire.mts_update != 0 ? wire.mts_update : wire.mts_create, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto qty = to_lots(split_sign(wire.amount_orig).magnitude, *base);
    const auto left = to_lots(split_sign(wire.amount).magnitude, *base);
    if (!qty.has_value() || qty->raw <= 0 || !left.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base->id;
    out.side = side_of(wire.amount_orig);
    out.exchange_client_order_id = {};
    if (!copy_id(wire.id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves = *left;
    switch (wire.status) {
    case WireOrderStatus::Active:
        out.status = left->raw < qty->raw ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
        break;
    case WireOrderStatus::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    case WireOrderStatus::Executed:
        out.status = OrderStatus::Filled;
        out.leaves = QtyLots::from_raw(0);
        break;
    case WireOrderStatus::Canceled:
        out.status = OrderStatus::Canceled;
        out.leaves = QtyLots::from_raw(0);
        break;
    case WireOrderStatus::Rejected:
        out.status = OrderStatus::Rejected;
        out.leaves = QtyLots::from_raw(0);
        break;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    out.ts_venue =
        ms_to_unix(wire.mts_update != 0 ? wire.mts_update : wire.mts_create, UnixNanos{0});
    return out;
}

std::optional<OrderEvent> map_execution(const WireExecution& wire,
                                        const exec::InstrumentTable& table,
                                        const OrderContext& context,
                                        UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    // exec_amount's sign is our side
    const auto executed = split_sign(wire.amount);
    const auto qty = to_lots(executed.magnitude, *base);
    const auto price = to_ticks(wire.price, *base);
    if (!qty.has_value() || qty->raw <= 0 || !price.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base->id;
    out.side = executed.negative ? Side::Sell : Side::Buy;
    out.exchange_client_order_id = context.client_order_id;
    if (!copy_id(wire.order_id, out.exchange_order_id) || !copy_exec_id(wire.id, out.exec_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = context.leaves;
    out.fee = fee_e9(wire.fee);
    // maker is 1 or -1; otherwise liquidity stays Unknown
    out.liquidity = wire.liquidity_known ? (wire.maker ? Liquidity::Maker : Liquidity::Taker)
                                         : Liquidity::Unknown;
    out.ts_event = ms_to_unix(wire.mts, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    if (wire.status != "ACTIVE") {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    // margin positions on spot pairs are not this adapter's; only perps are reported
    if (base->id.market() != MarketType::Perp) {
        return std::nullopt;
    }
    const auto amount = split_sign(wire.amount);
    const auto size = to_lots(amount.magnitude, *base);
    if (!size.has_value() || size->raw == 0) {
        return std::nullopt; // absent means flat; a zero row states nothing
    }

    exec::PositionReport out{};
    out.instrument = base->id;
    // negative is short
    out.net_qty = QtyLots::from_raw(amount.negative ? -size->raw : size->raw);
    if (const auto entry = to_ticks(wire.base_price, *base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    out.ts_venue = ms_to_unix(wire.mts_update, UnixNanos{0});
    return out;
}

} // namespace raikiri::venues::bitfinex
