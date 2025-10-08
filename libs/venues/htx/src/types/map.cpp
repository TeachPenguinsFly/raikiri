#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/venues/htx/map.hpp>

#include <charconv>
#include <limits>
#include <optional>
#include <variant>

namespace raikiri::venues::htx {

namespace {

// 1 is listed and tradable; 3 is a row on its way out that still ships usable
// numbers, so only the status keeps it out.
constexpr std::int64_t k_contract_listed = 1;

// Guards the int128 accumulator and the power-of-ten loops; no live HTX token
// comes close.
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

constexpr int k_seconds_per_hour = 3600;

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

// Splits a JSON numeric token into an exact mantissa and a base-10 exponent.
// HTX spells price_tick both ways within ONE response, so reading only plain
// decimals would silently lose whichever rows took the other form.
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
        value /= 10;
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

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means HTX changed an instrument's precision, so the message is
// dropped rather than rounded — rounding is how an order ends up one tick away
// from the intended price.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = exact_e9(text);
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

// What FixedE9 carries, and therefore the finest spot precision this adapter
// can represent.
constexpr std::uint32_t k_e9_digits = 9;

// A step of 10^-digits, in e9 fixed point.
std::int64_t power_of_ten(std::uint32_t digits) noexcept {
    std::int64_t step = k_e9;
    for (std::uint32_t i = 0; i < digits; ++i) {
        step /= 10;
    }
    return step;
}

// HTX names the order type and the time in force in ONE token on both APIs:
// the swap's order_price_type ("limit", "post_only", "opponent_ioc") and
// spot's type ("buy-limit-maker", "sell-ioc"). Reading them apart is this
// file's job, not the wire parser's.
OrderType order_type_of(std::string_view spelling) noexcept {
    if (spelling.find("market") != std::string_view::npos || spelling.starts_with("opponent") ||
        spelling.starts_with("optimal") || spelling.starts_with("lightning")) {
        return OrderType::Market;
    }
    return OrderType::Limit;
}

TimeInForce tif_of(std::string_view spelling) noexcept {
    if (spelling.find("fok") != std::string_view::npos) {
        return TimeInForce::Fok;
    }
    if (spelling.find("ioc") != std::string_view::npos) {
        return TimeInForce::Ioc;
    }
    return TimeInForce::Gtc;
}

bool post_only(std::string_view spelling) noexcept {
    return spelling == "post_only" || spelling.ends_with("limit-maker");
}

// Fees are neither a price nor a size, so they truncate at 1e-9 where those
// refuse: a fee padded past nine decimals must not cost us the whole fill.
//
// The swap API writes a CHARGE as a negative number and a maker rebate as a
// positive one; spot writes a charge positive. One convention reaches the OMS,
// so the swap's is flipped here rather than at every call site.
FixedE9 fee_e9(std::string_view text, MarketType market) noexcept {
    const auto value = rate_e9(text);
    if (!value.has_value()) {
        return FixedE9::from_raw(0);
    }
    return market == MarketType::Spot ? *value : FixedE9::from_raw(-value->raw);
}

bool copy_ids(std::string_view client_text,
              std::string_view order_text,
              MarketType market,
              OrderEvent& out) noexcept {
    const auto identity = map_identity(client_text, order_text, market);
    if (!identity.has_value()) {
        return false;
    }
    if (identity->client_order_id.valid()) {
        out.client_order_id = identity->client_order_id;
    }
    if (!identity->exchange_client_order_id.empty()) {
        out.exchange_client_order_id = identity->exchange_client_order_id;
    }
    out.exchange_order_id = identity->exchange_order_id;
    return true;
}

bool copy_exec_id(std::string_view text, OrderEvent& out) noexcept {
    const auto id = ExecId::from(text);
    if (!id.has_value() || id->empty()) {
        return false; // a fill with no dedupe key is one we could count twice
    }
    out.exec_id = *id;
    return true;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> exact_e9(std::string_view token) {
    if (token.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(token);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/true) : std::nullopt;
}

std::optional<FixedE9> rate_e9(std::string_view token) {
    if (token.empty()) {
        return std::nullopt;
    }
    const auto parts = decompose(token);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
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
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.amount, base);
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
map_funding(const WireFunding& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = rate_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base_of(*instrument).id;
    out.rate = *rate;
    if (const auto predicted = rate_e9(wire.estimated_rate); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    // settlement_time, not funding_time — see wire.hpp.
    out.next_funding_time = ms_to_unix(wire.settlement_ms, UnixNanos{0});
    if (const auto* perp = std::get_if<PerpInstrument>(instrument); perp != nullptr) {
        out.interval_s = perp->funding_interval_s;
    }
    out.ts_event = ms_to_unix(wire.funding_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireDetail& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.amount.empty() && wire.trade_turnover.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    // `vol` on the same tick is the contract count and is not read here.
    out.volume_24h_base = rate_e9(wire.amount).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = rate_e9(wire.trade_turnover).value_or(FixedE9::from_raw(0));
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot>
map_depth(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    const auto fill = [&base](std::span<const WireLevel> src,
                              std::array<BookLevel, k_book_levels>& dst) {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, base);
            const auto qty = to_lots(level.qty, base);
            // Truncating is safe on a snapshot venue and only there: the next
            // whole book lands 100 ms later, so a refused level heals itself.
            if (!price.has_value() || !qty.has_value()) {
                break;
            }
            if (qty->raw == 0) {
                continue; // a resting size of zero is no level at all
            }
            dst[n] = {.price = *price, .qty = *qty};
            ++n;
        }
        return n;
    };

    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    out.venue_seq = wire.mrid;
    out.bid_count = fill(wire.bids, out.bids);
    out.ask_count = fill(wire.asks, out.asks);
    if (out.bid_count == 0 || out.ask_count == 0) {
        return std::nullopt;
    }
    return out;
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    if (wire.contract_status != k_contract_listed) {
        return std::nullopt;
    }
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.business_type != "swap" || wire.contract_type != "swap") {
        return std::nullopt;
    }

    const auto tick = exact_e9(wire.price_tick);
    const auto size = exact_e9(wire.contract_size);
    if (!tick.has_value() || tick->raw <= 0 || !size.has_value() || size->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.contract_code);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Htx, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = FixedE9::from_raw(k_e9);
    base.contract_value = *size;
    // Unconditional because the catalogue comes from /linear-swap-api. HTX's
    // coin-margined swaps live under /swap-api and are inverse; reaching for
    // them means deciding this per row.
    base.kind = ContractKind::Linear;
    std::uint32_t interval_s = 0;
    if (wire.settlement_period_h > 0) {
        interval_s = static_cast<std::uint32_t>(wire.settlement_period_h * k_seconds_per_hour);
    }
    return PerpInstrument{base, interval_s};
}

std::optional<Instrument> map_spot_symbol(const WireSpotSymbol& wire) {
    if (wire.state != "online" || wire.api_trading != "enabled") {
        return std::nullopt;
    }
    // Nine is what FixedE9 carries. A pair asking for a tenth decimal — atpbtc
    // does, live — cannot be represented, and a step rounded up to 1e-9 would
    // put every order on that pair one tick off the grid it was priced on.
    if (wire.price_precision > k_e9_digits || wire.amount_precision > k_e9_digits) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Htx, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = FixedE9::from_raw(power_of_ten(wire.price_precision));
    base.lot_size = FixedE9::from_raw(power_of_ten(wire.amount_precision));
    if (const auto min_value = exact_e9(wire.min_order_value); min_value.has_value()) {
        base.min_notional = *min_value;
    }
    // A spot pair has no contract: one unit of base is one unit of base.
    // Linear is nonetheless what the kind must say — `validate` refuses
    // ContractKind::Unknown, so leaving it there had InstrumentTable::add
    // reject all 587 listed pairs and the gateway load a catalogue of zero.
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

// Order entry ------------------------------------------------------------

std::optional<OrderIdentity>
map_identity(std::string_view client_order_id, std::string_view order_id, MarketType market) {
    OrderIdentity out{};
    if (!client_order_id.empty()) {
        if (market == MarketType::Spot) {
            const auto id = ExchangeClientOrderId::from(client_order_id);
            if (!id.has_value()) {
                return std::nullopt; // longer than a ULID: not one of ours
            }
            out.exchange_client_order_id = *id;
        } else {
            std::uint64_t raw = 0;
            const auto* const end = client_order_id.data() + client_order_id.size();
            const auto [stop, ec] = std::from_chars(client_order_id.data(), end, raw);
            // HTX writes 0 for an order placed without a client id, and any
            // non-numeric spelling is a field we did not put there.
            if (ec == std::errc{} && stop == end && raw != 0) {
                out.client_order_id = ClientOrderId{raw};
            }
        }
    }
    if (!order_id.empty()) {
        const auto id = ExchangeOrderId::from(order_id);
        if (!id.has_value()) {
            return std::nullopt; // absent is survivable; truncated is not
        }
        out.exchange_order_id = *id;
    }
    return out;
}

std::optional<QtyLots> order_leaves(const WireOrder& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    if (!wire.left_qty.empty()) {
        return to_lots(wire.left_qty, base);
    }
    const auto total = to_lots(wire.qty, base);
    const auto filled = to_lots(wire.filled_qty, base);
    if (!total.has_value()) {
        return std::nullopt;
    }
    // An absent trade_volume is zero filled, which is what an untouched order
    // reports; a present one that will not convert is a schema event.
    if (!filled.has_value()) {
        return wire.filled_qty.empty() ? total : std::nullopt;
    }
    if (filled->raw > total->raw) {
        return std::nullopt;
    }
    return QtyLots::from_raw(total->raw - filled->raw);
}

std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    OrderEventType type{};
    switch (wire.state) {
    case WireOrderState::Accepted:
        type = OrderEventType::Accepted;
        break;
    case WireOrderState::Canceled:
        type = OrderEventType::Canceled;
        break;
    case WireOrderState::Rejected:
        type = OrderEventType::Rejected;
        break;
    case WireOrderState::PartiallyFilled:
    case WireOrderState::Filled:
        // The fill that moved leaves already said this, with the exec id and
        // the fee attached. Saying it twice attributes one fill to two events.
        return std::nullopt;
    case WireOrderState::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = wire.sell ? Side::Sell : Side::Buy;
    out.order_type = order_type_of(wire.order_price_type);
    out.tif = tif_of(wire.order_price_type);
    if (post_only(wire.order_price_type)) {
        out.flags = OrderFlags::PostOnly;
    }
    if (!copy_ids(wire.client_order_id, wire.order_id, base.id.market(), out)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.qty, base); qty.has_value()) {
        out.qty = *qty;
    }
    out.leaves_qty = order_leaves(wire, table).value_or(QtyLots::from_raw(0));
    // A cancel leaves nothing working whatever the last quantity frame said.
    if (type == OrderEventType::Canceled || type == OrderEventType::Rejected) {
        out.leaves_qty = QtyLots::from_raw(0);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_fill(const WireOrder& order,
                                   const WireFill& fill,
                                   const exec::InstrumentTable& table,
                                   QtyLots leaves,
                                   UnixNanos ts_init) {
    const auto* instrument = table.find(order.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(fill.price, base);
    const auto qty = to_lots(fill.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = order.sell ? Side::Sell : Side::Buy;
    out.order_type = order_type_of(order.order_price_type);
    out.tif = tif_of(order.order_price_type);
    out.liquidity = fill.maker ? Liquidity::Maker : Liquidity::Taker;
    if (!copy_ids(order.client_order_id, order.order_id, base.id.market(), out) ||
        !copy_exec_id(fill.exec_id, out)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = leaves.is_undef() ? QtyLots::from_raw(0) : leaves;
    out.fee = fee_e9(fill.fee, base.id.market());
    out.ts_event = ms_to_unix(fill.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_user_trade(const WireUserTrade& wire,
                                         const exec::InstrumentTable& table,
                                         QtyLots leaves,
                                         ClientOrderId known_client_id,
                                         const ExchangeClientOrderId& known_ulid,
                                         UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.fill.price, base);
    const auto qty = to_lots(wire.fill.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = wire.sell ? Side::Sell : Side::Buy;
    out.liquidity = wire.fill.maker ? Liquidity::Maker : Liquidity::Taker;
    // The caller's cache first, then whatever the frame states: a REST match
    // row carries no client id at all on spot, and an event with none routes
    // by the venue order id or not at all.
    out.client_order_id = known_client_id;
    out.exchange_client_order_id = known_ulid;
    if (!copy_ids(wire.client_order_id, wire.order_id, base.id.market(), out) ||
        !copy_exec_id(wire.fill.exec_id, out)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    if (!wire.left_qty.empty()) {
        if (const auto left = to_lots(wire.left_qty, base); left.has_value()) {
            leaves = *left;
        }
    }
    out.leaves_qty = leaves.is_undef() ? QtyLots::from_raw(0) : leaves;
    out.fee = fee_e9(wire.fill.fee, base.id.market());
    out.ts_event = ms_to_unix(wire.fill.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = to_lots(wire.qty, base);
    const auto leaves = order_leaves(wire, table);
    if (!qty.has_value() || !leaves.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = wire.sell ? Side::Sell : Side::Buy;
    const auto identity = map_identity(wire.client_order_id, wire.order_id, base.id.market());
    if (!identity.has_value()) {
        return std::nullopt;
    }
    // A perp report carries no exchange_client_order_id at all: the ULID the
    // OMS minted was never on the wire, and the venue order id is what the
    // reconciler matches on there.
    out.exchange_client_order_id = identity->exchange_client_order_id;
    out.exchange_order_id = identity->exchange_order_id;
    switch (wire.state) {
    case WireOrderState::Accepted:
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderState::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    case WireOrderState::Filled:
        out.status = OrderStatus::Filled;
        break;
    case WireOrderState::Canceled:
        out.status = OrderStatus::Canceled;
        break;
    case WireOrderState::Rejected:
        out.status = OrderStatus::Rejected;
        break;
    case WireOrderState::Unknown:
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves = *leaves;
    out.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.contract_code);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    // HTX states the size as a MAGNITUDE and the side as a word beside it, so
    // a reader that forgets `direction` reports every short as a long.
    const auto size = to_lots(wire.volume, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = wire.sell ? QtyLots::from_raw(-size->raw) : *size;
    if (const auto entry = to_ticks(wire.cost_hold, base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    out.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{});
    return out;
}

} // namespace raikiri::venues::htx
