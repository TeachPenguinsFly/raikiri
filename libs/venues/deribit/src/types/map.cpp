#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/options/convention.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>

#include <array>
#include <charconv>
#include <limits>
#include <optional>

namespace raikiri::venues::deribit {

namespace {

// the rate's unit, not a payment cadence
constexpr std::uint32_t k_funding_basis_s = 8 * 60 * 60;

// deribit sets perpetual expiry to 32503708800000 (year 3000); unixnanos wraps past 2554
constexpr std::uint64_t k_max_expiry_ms = std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL;

// real exponents are in [-5, 5]; this only bounds the shift
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

// a negative shift must divide exactly; extra precision is refused
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

// a remainder means the instrument's precision changed, so the value is dropped
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

// 24h aggregates are amounts off the lot grid, truncated below 1e-9
std::optional<FixedE9> to_amount_e9(std::string_view token) {
    return token.empty() ? std::nullopt : parse_amount_e9(token);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 || ms > k_max_expiry_ms ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo> map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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

namespace {

constexpr options::Convention k_convention{.iv = options::IvScale::Percent};

[[nodiscard]] std::optional<FixedE9> figure(std::string_view text) {
    return text.empty() ? std::nullopt : parse_number_e9(text);
}

} // namespace

std::optional<OptionGreeks> map_greeks(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!has_greeks(wire)) {
        return std::nullopt; // a perpetual's ticker carries no greeks object
    }
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const options::VenueGreeks venue{.delta = figure(wire.delta),
                                     .gamma = figure(wire.gamma),
                                     .vega = figure(wire.vega),
                                     .theta = figure(wire.theta),
                                     .rho = figure(wire.rho),
                                     .mark_iv = figure(wire.mark_iv),
                                     .bid_iv = figure(wire.bid_iv),
                                     .ask_iv = figure(wire.ask_iv),
                                     .underlying_price = figure(wire.underlying_price),
                                     .mark_price = figure(wire.mark_price),
                                     .index_price = figure(wire.index_price)};
    return options::normalise(base_of(*instrument).id, venue, k_convention, ms_to_unix(wire.ts_ms, ts_init), ts_init);
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
    out.accrual = FundingAccrual::Continuous;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // absent stats is not zero volume
    if (wire.stats_volume.empty() && wire.stats_volume_usd.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = to_amount_e9(wire.stats_volume).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = parse_turnover_e6(wire.stats_volume_usd).value_or(FixedE6::from_raw(0));
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

namespace {

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

// deribit has no expired state; tif tells an ioc remainder from a cancel
bool ends_by_expiry(std::string_view tif) noexcept {
    return tif == "immediate_or_cancel" || tif == "fill_or_kill";
}

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

void stamp(OrderEvent& event, const WireOrder& wire, const InstrumentBase& base, UnixNanos ts_init) {
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
        // a partially filled order cannot go back to accepted
        if (leaves->raw != qty->raw) {
            return std::nullopt;
        }
        event.type = OrderEventType::Accepted;
        break;
    }
    case OrderState::Filled:
        // fills arrive as trades; a filled event here would count them twice
        return std::nullopt;
    case OrderState::Rejected:
        event.type = OrderEventType::Rejected;
        break;
    case OrderState::Cancelled:
        event.type = ends_by_expiry(wire.tif) ? OrderEventType::Expired : OrderEventType::Canceled;
        break;
    case OrderState::Triggered:
        event.type = OrderEventType::Triggered;
        break;
    case OrderState::Untriggered:
        // this adapter places no trigger orders
        return std::nullopt;
    }
    stamp(event, wire, base, ts_init);
    event.leaves_qty = event.type == OrderEventType::Rejected ? QtyLots::from_raw(0) : *leaves;
    return event;
}

std::optional<OrderEvent>
map_fill(const WireUserTrade& wire, const exec::InstrumentTable& table, QtyLots leaves, UnixNanos ts_init) {
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
        // leaves must be definite before the oms subtracts it
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
    // fee is in the base coin
    if (const auto fee = parse_number_e9(wire.fee); fee.has_value()) {
        event.fee = *fee;
    }
    event.liquidity = wire.liquidity_stated ? (wire.maker ? Liquidity::Maker : Liquidity::Taker) : Liquidity::Unknown;
    event.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    event.ts_init = ts_init;
    return event;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire, const exec::InstrumentTable& table) {
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
        report.status = leaves->raw == qty->raw ? OrderStatus::Accepted : OrderStatus::PartiallyFilled;
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

std::optional<exec::FillReport> map_fill_report(const WireUserTrade& wire, const exec::InstrumentTable& table) {
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
    report.liquidity = wire.liquidity_stated ? (wire.maker ? Liquidity::Maker : Liquidity::Taker) : Liquidity::Unknown;
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

namespace {

// BTC-PERPETUAL settles in BTC, BTC_USDC-PERPETUAL in USDC
std::string_view settle_asset(const InstrumentBase& base) noexcept {
    const auto prefix = currency_of(base.exchange_symbol.view());
    const auto underscore = prefix.find('_');
    return underscore == std::string_view::npos ? prefix : prefix.substr(underscore + 1);
}

} // namespace

std::optional<LedgerEntry>
map_funding_settlement(const WireLogEntry& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.type != "settlement") {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr || !std::holds_alternative<PerpInstrument>(*instrument)) {
        return std::nullopt; // a dated future settles too, with no funding
    }
    const auto& base = base_of(*instrument);
    if (wire.currency.empty() || wire.currency != settle_asset(base)) {
        return std::nullopt;
    }
    // funding pnl, so positive was received; finer than 1e-9 of a coin is truncated
    const auto amount = wire.total_interest_pl.empty() ? std::nullopt : parse_amount_e9(wire.total_interest_pl);
    if (!amount.has_value() || amount->raw == 0 || wire.ts_ms == 0) {
        return std::nullopt;
    }
    LedgerEntry out{};
    out.instrument = base.id;
    out.amount = *amount;
    out.kind = LedgerKind::Funding;
    out.settled_at = UnixNanos{wire.ts_ms * 1'000'000ULL};
    std::array<char, 24> id{};
    if (const auto [end, ec] = std::to_chars(id.data(), id.data() + id.size(), wire.id); ec == std::errc{}) {
        if (const auto ref = ExecId::from(std::string_view{id.data(), static_cast<std::size_t>(end - id.data())});
            ref.has_value()) {
            out.reference = *ref;
        }
    }
    out.ts_init = ts_init;
    return out;
}

bool collect_funding_settlements(std::span<const WireLogEntry> entries,
                                 const exec::InstrumentTable& table,
                                 UnixNanos ts_init,
                                 std::unordered_set<std::uint64_t>& seen,
                                 std::vector<LedgerEntry>& out) {
    bool fresh = false;
    for (const auto& entry : entries) {
        if (!seen.insert(entry.id).second) {
            continue;
        }
        fresh = true;
        if (auto charge = map_funding_settlement(entry, table, ts_init); charge.has_value()) {
            out.push_back(*charge);
        }
    }
    return fresh;
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
    if (!tick.has_value() || !lot.has_value() || !contract.has_value() || tick->raw <= 0 || lot->raw <= 0 ||
        contract->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.instrument_name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Deribit, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = *lot;
    // lot_size is already min_trade_amount in deribit's amount unit,
    // so storing contract_size as well would multiply the notional twice
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Inverse;

    if (market == MarketType::Option) {
        // options quote premium and size in the base coin, so the notional is linear
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
        // premium is in the coin, greeks in usd
        return OptionInstrument{.base = base,
                                .strike = PriceTicks::from_raw(*strike),
                                .expiry = ms_to_unix(wire.expiration_ms, UnixNanos{0}),
                                .put_call = wire.option_type == "put" ? PutCall::Put : PutCall::Call,
                                .premium = OptionUnit::Base,
                                .greeks = OptionUnit::Quote};
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
