#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/coinbase/catalogue.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/market_data.hpp>
#include <raikiri/venues/coinbase/orders.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::coinbase {

namespace {

// exact against the instrument step; a remainder drops the message
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

// 24-hour aggregates are amounts, not lot multiples; truncation below 1e-9 rounds down
std::optional<FixedE9> to_amount_e9(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

// hand-rolled iso-8601 parser: strptime is locale-sensitive and drops sub-second digits;
// the fraction is 3, 6 or 9 digits

constexpr std::size_t k_head_len = 19; // "YYYY-MM-DDTHH:MM:SS"

std::optional<std::int64_t> digits_at(std::string_view text, std::size_t at, std::size_t count) {
    std::int64_t out = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[at + i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        out = out * 10 + (c - '0');
    }
    return out;
}

// howard hinnant's days_from_civil; avoids <chrono> calendar types and libstdc++'s tzdb
constexpr std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
    y -= static_cast<std::int64_t>(m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146'097 + doe - 719'468;
}

// UnixNanos runs out in 2554; guard on seconds before multiplying
constexpr std::int64_t k_max_epoch_s = 18'446'744'073LL;

} // namespace

std::optional<UnixNanos> parse_iso8601(std::string_view text) noexcept {
    if (text.size() < k_head_len) {
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto year = digits_at(text, 0, 4);
    const auto month = digits_at(text, 5, 2);
    const auto day = digits_at(text, 8, 2);
    const auto hour = digits_at(text, 11, 2);
    const auto minute = digits_at(text, 14, 2);
    const auto second = digits_at(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }

    std::int64_t frac_ns = 0;
    std::size_t i = k_head_len;
    if (i < text.size() && text[i] == '.') {
        ++i;
        std::int64_t scale = 100'000'000; // the first fractional digit is 1e8 ns
        std::size_t seen = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (scale > 0) {
                frac_ns += static_cast<std::int64_t>(text[i] - '0') * scale;
                scale /= 10;
            }
            ++i;
            ++seen;
        }
        if (seen == 0) {
            return std::nullopt;
        }
    }
    // coinbase only sends 'Z'
    if (i != text.size() && !(text[i] == 'Z' && i + 1 == text.size())) {
        return std::nullopt;
    }

    const std::int64_t secs = days_from_civil(*year, *month, *day) * 86'400 + *hour * 3'600 + *minute * 60 + *second;
    if (secs < 0 || secs > k_max_epoch_s) {
        return std::nullopt;
    }
    return UnixNanos{static_cast<std::uint64_t>(secs) * 1'000'000'000ULL + static_cast<std::uint64_t>(frac_ns)};
}

UnixNanos iso_to_unix(std::string_view text, UnixNanos fallback) noexcept {
    const auto parsed = parse_iso8601(text);
    return parsed.has_value() ? *parsed : fallback;
}

std::optional<Bbo> map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto bid = to_ticks(wire.best_bid, base);
    const auto ask = to_ticks(wire.best_ask, base);
    const auto bid_qty = to_lots(wire.best_bid_size, base);
    const auto ask_qty = to_lots(wire.best_ask_size, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = iso_to_unix(wire.time, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireProductVolume& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // a delisted product states neither column
    const auto volume = to_amount_e9(wire.volume_24h);
    const auto quote = parse_turnover_e6(wire.quote_volume_24h);
    if (!volume.has_value() || !quote.has_value()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = *volume;
    out.volume_24h_quote = *quote;
    out.ts_event = UnixNanos{};
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireMatch& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.size, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base.id;
    out.price = *price;
    out.qty = *qty;
    // match side is the maker's; flip it
    out.aggressor = wire.maker_sold ? Side::Buy : Side::Sell;
    out.ts_event = iso_to_unix(wire.time, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

namespace {

// coinbase's order states
enum class OrderState : std::uint8_t { Open, Pending, Active, Rejected, Done };

std::optional<OrderState> state_of(std::string_view text) noexcept {
    if (text == "open") {
        return OrderState::Open;
    }
    if (text == "pending") {
        return OrderState::Pending;
    }
    if (text == "active") {
        return OrderState::Active;
    }
    if (text == "rejected") {
        return OrderState::Rejected;
    }
    if (text == "done") {
        return OrderState::Done;
    }
    return std::nullopt;
}

std::optional<QtyLots> leaves_of(const WireOrder& wire, const InstrumentBase& base) {
    const auto size = to_lots(wire.size, base);
    if (!size.has_value()) {
        return std::nullopt;
    }
    if (wire.filled_size.empty()) {
        return size;
    }
    const auto filled = to_lots(wire.filled_size, base);
    if (!filled.has_value() || filled->raw > size->raw) {
        return std::nullopt;
    }
    return QtyLots::from_raw(size->raw - filled->raw);
}

} // namespace

std::optional<QtyLots> order_leaves(const WireOrder& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    return leaves_of(wire, base_of(*instrument));
}

std::optional<OrderEvent> map_order_event(const WireOrder& wire,
                                          const exec::InstrumentTable& table,
                                          const ExchangeClientOrderId& client_id,
                                          UnixNanos ts_init) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt; // a product this process never loaded
    }
    const auto state = state_of(wire.status);
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
    case OrderState::Open:
    case OrderState::Pending:
    case OrderState::Active:
        // pending is still an acknowledgement; a second Accepted is idempotent
        event.type = OrderEventType::Accepted;
        break;
    case OrderState::Rejected:
        event.type = OrderEventType::Rejected;
        break;
    case OrderState::Done:
        if (wire.done_reason == "filled") {
            // fills come from executions this adapter cannot observe
            return std::nullopt;
        }
        event.type = OrderEventType::Canceled;
        break;
    }

    event.side = wire.sell ? Side::Sell : Side::Buy;
    event.instrument = base.id;
    event.exchange_client_order_id = client_id;
    if (const auto id = ExchangeOrderId::from(wire.id); id.has_value()) {
        event.exchange_order_id = *id;
    }
    // a market order carries no price; it stays undef
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        event.price = *price;
    }
    if (const auto qty = to_lots(wire.size, base); qty.has_value()) {
        event.qty = *qty;
    }
    event.leaves_qty = event.type == OrderEventType::Rejected ? QtyLots::from_raw(0) : *leaves;
    const auto stamped = wire.done_at.empty() ? wire.created_at : wire.done_at;
    event.ts_event = iso_to_unix(stamped, ts_init);
    event.ts_init = ts_init;
    return event;
}

std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table, const ExchangeClientOrderId& client_id) {
    const auto* instrument = table.find(wire.product_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto state = state_of(wire.status);
    if (!state.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = to_lots(wire.size, base);
    const auto leaves = leaves_of(wire, base);
    if (!qty.has_value() || !leaves.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport report{};
    switch (*state) {
    case OrderState::Open:
    case OrderState::Pending:
    case OrderState::Active:
        report.status = leaves->raw == qty->raw ? OrderStatus::Accepted : OrderStatus::PartiallyFilled;
        break;
    case OrderState::Rejected:
        report.status = OrderStatus::Rejected;
        break;
    case OrderState::Done:
        report.status = wire.done_reason == "filled" ? OrderStatus::Filled : OrderStatus::Canceled;
        break;
    }
    report.exchange_client_order_id = client_id;
    if (const auto id = ExchangeOrderId::from(wire.id); id.has_value()) {
        report.exchange_order_id = *id;
    }
    report.instrument = base.id;
    report.side = wire.sell ? Side::Sell : Side::Buy;
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        report.price = *price;
    }
    report.qty = *qty;
    report.leaves = *leaves;
    const auto stamped = wire.done_at.empty() ? wire.created_at : wire.done_at;
    report.ts_venue = iso_to_unix(stamped, UnixNanos{0});
    return report;
}

std::optional<Instrument> map_instrument(const WireProduct& wire, MarketType market) {
    if (market != MarketType::Spot) {
        return std::nullopt;
    }
    if (wire.status != "online" || wire.trading_disabled) {
        return std::nullopt;
    }

    const auto tick = parse_decimal_e9(wire.quote_increment);
    const auto step = parse_decimal_e9(wire.base_increment);
    if (!tick.has_value() || !step.has_value() || tick->raw <= 0 || step->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.id);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Coinbase, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *step;
    if (const auto min_funds = parse_decimal_e9(wire.min_market_funds); min_funds.has_value() && min_funds->raw > 0) {
        base.min_notional = *min_funds;
    }
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

} // namespace raikiri::venues::coinbase
