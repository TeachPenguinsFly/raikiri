#include <raikiri/domain/symbol.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/venues/bybit/map.hpp>

#include <charconv>

namespace raikiri::venues::bybit {

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

// fees and rates keep 1e9 scale, not instrument ticks
FixedE9 to_e9(std::string_view text) {
    if (text.empty()) {
        return FixedE9::from_raw(0);
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? *value : FixedE9::from_raw(0);
}

// balances come at the venue's resolution, which the exact reader would refuse
std::optional<FixedE9> to_amount(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

// epoch ms as digits; zero means no usable time
std::uint64_t digits_to_ms(std::string_view text) noexcept {
    if (text.empty()) {
        return 0;
    }
    std::uint64_t out = 0;
    const auto* const end = text.data() + text.size();
    const auto [stop, ec] = std::from_chars(text.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
}

bool copy_id(std::string_view text, ExchangeOrderId& out) {
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

// an id that does not fit cannot be ours; the event stays unattributed
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

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

Side side_from(std::string_view text) noexcept {
    return text == "Sell" ? Side::Sell : Side::Buy;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt; // a symbol we never subscribed to
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
    out.aggressor = wire.buyer_aggressor ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, FundingMemo& memo, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    // each field only where the frame stated it
    bool told = false;
    if (!wire.funding_rate.empty()) {
        memo.rate = to_e9(wire.funding_rate);
        told = true;
    }
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        memo.mark = *mark;
        told = true;
    }
    if (const auto ms = digits_to_ms(wire.next_funding_ms); ms != 0) {
        memo.next_funding = UnixNanos{ms * 1'000'000ULL};
        told = true;
    }
    if (const auto* perp = std::get_if<PerpInstrument>(instrument); perp != nullptr) {
        memo.interval_s = perp->funding_interval_s;
    }
    if (!told) {
        return std::nullopt; // the frame was about the touch, not about funding
    }

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = memo.rate;
    out.predicted_rate = FixedE9::from_raw(0); // bybit publishes no forecast here
    out.mark_price = memo.mark;
    out.next_funding_time = memo.next_funding;
    out.interval_s = memo.interval_s;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // both volumes absent means unchanged
    if (wire.volume_24h.empty() && wire.turnover_24h.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = to_e9(wire.volume_24h);
    out.volume_24h_quote = parse_turnover_e6(wire.turnover_24h).value_or(FixedE6::undef());
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent>
map_order_event(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    OrderEventType type{};
    switch (wire.status) {
    case WireOrderStatus::New:
    case WireOrderStatus::Untriggered:
        type = OrderEventType::Accepted;
        break;
    case WireOrderStatus::Triggered:
        type = OrderEventType::Triggered;
        break;
    case WireOrderStatus::Rejected:
        type = OrderEventType::Rejected;
        break;
    case WireOrderStatus::Cancelled:
    case WireOrderStatus::Deactivated:
        type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::PartiallyFilledCanceled:
        type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::PartiallyFilled:
    case WireOrderStatus::Filled:
        return std::nullopt;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.order_type = wire.order_type == "Market" ? OrderType::Market : OrderType::Limit;
    if (!copy_client_id(wire.order_link_id, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.qty, base); qty.has_value()) {
        out.qty = *qty;
    }
    if (const auto leaves = to_lots(wire.leaves_qty, base); leaves.has_value()) {
        out.leaves_qty = *leaves;
    }
    out.ts_event = ms_to_unix(wire.updated_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

// sign flipped from bybit's fee convention to the account's: positive received
std::optional<LedgerEntry>
map_funding_charge(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!is_funding_execution(wire)) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto fee = parse_decimal_e9(wire.exec_fee);
    if (!fee.has_value()) {
        return std::nullopt; // an unreadable figure is not a zero one
    }
    LedgerEntry out{};
    out.instrument = base_of(*instrument).id;
    out.amount = FixedE9{-fee->raw};
    out.settled_at = ms_to_unix(wire.exec_ms, ts_init);
    if (auto id = ExecId::from(wire.exec_id); id.has_value()) {
        out.reference = *id;
    }
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent>
map_execution(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // only Trade rows are fills; funding, settlement and adl rows carry a real qty and price
    if (!wire.exec_type.empty() && wire.exec_type != "Trade") {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.exec_price, base);
    const auto qty = to_lots(wire.exec_qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.liquidity = wire.is_maker ? Liquidity::Maker : Liquidity::Taker;
    if (!copy_client_id(wire.order_link_id, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (!wire.exec_id.empty()) {
        const auto exec_id = ExecId::from(wire.exec_id);
        if (!exec_id.has_value()) {
            return std::nullopt; // dedupe key we cannot store is not usable
        }
        out.exec_id = *exec_id;
    }
    out.price = *price;
    out.qty = *qty;
    if (const auto leaves = to_lots(wire.leaves_qty, base); leaves.has_value()) {
        out.leaves_qty = *leaves;
    } else {
        out.leaves_qty = QtyLots::from_raw(0);
    }
    out.fee = to_e9(wire.exec_fee);
    out.ts_event = ms_to_unix(wire.exec_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    if (!copy_client_id(wire.order_link_id, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    switch (wire.status) {
    case WireOrderStatus::New:
    case WireOrderStatus::Untriggered:
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderStatus::Triggered:
        out.status = OrderStatus::Triggered;
        break;
    case WireOrderStatus::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    default:
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.qty, base); qty.has_value()) {
        out.qty = *qty;
    }
    if (const auto leaves = to_lots(wire.leaves_qty, base); leaves.has_value()) {
        out.leaves = *leaves;
    }
    out.ts_venue = ms_to_unix(wire.updated_ms, UnixNanos{});
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
    out.net_qty = wire.side == "Sell" ? QtyLots::from_raw(-size->raw) : *size;
    if (const auto avg = to_ticks(wire.avg_price, base); avg.has_value()) {
        out.avg_entry = *avg;
    }
    out.ts_venue = ms_to_unix(wire.updated_ms, UnixNanos{});
    return out;
}

std::optional<BalanceReport> map_coin_balance(const WireCoinBalance& wire, UnixNanos ts_venue) {
    const auto asset = to_asset_code(wire.coin);
    if (!asset.has_value() || asset->empty()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    // fields stay undef where bybit sent an empty string; availableToWithdraw is empty on unified accounts
    if (const auto total = to_amount(wire.wallet_balance); total.has_value()) {
        out.total = *total;
    }
    if (const auto available = to_amount(wire.available_to_withdraw); available.has_value()) {
        out.available = *available;
    }
    if (const auto unrealized = to_amount(wire.unrealised_pnl); unrealized.has_value()) {
        out.unrealized = *unrealized;
    }
    out.ts_venue = ts_venue;
    return out;
}

namespace {

[[nodiscard]] AssetClass asset_class_of(std::string_view symbol_type) noexcept {
    if (symbol_type.empty() || symbol_type == "innovation" || symbol_type == "adventure") {
        return AssetClass::Crypto;
    }
    if (symbol_type == "stock" || symbol_type == "ETF") {
        return AssetClass::Equity;
    }
    if (symbol_type == "commodity") {
        return AssetClass::Commodity;
    }
    if (symbol_type == "forex") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

} // namespace

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (wire.status != "Trading") {
        return std::nullopt; // delisted or pre-launch: not tradable today
    }
    // the linear catalogue includes dated futures; this adapter serves perps only
    if (market == MarketType::Perp && !wire.contract_type.empty() && !wire.contract_type.ends_with("Perpetual")) {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.qty_step);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        // later price conversions divide by these
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bybit, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    // rows without a stated pair are dropped
    const auto canonical = canonical_symbol(wire.base_coin, wire.quote_coin);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = to_e9(wire.min_notional);
    base.contract_value = *parse_decimal_e9("1"); // linear perps quote in base units
    base.kind = ContractKind::Linear;
    base.asset_class = asset_class_of(wire.symbol_type);
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }
    // the venue's interval in minutes; 0 stays 0
    PerpInstrument perp{base, static_cast<std::uint32_t>(wire.funding_interval_min * 60)};
    // an unparsable cap stays undef
    if (const auto cap = parse_decimal_e9(wire.funding_cap); cap.has_value()) {
        perp.funding_cap = *cap;
    }
    if (const auto floor = parse_decimal_e9(wire.funding_floor); floor.has_value()) {
        perp.funding_floor = *floor;
    }
    return perp;
}

} // namespace raikiri::venues::bybit
