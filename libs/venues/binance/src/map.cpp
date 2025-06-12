#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/binance/map.hpp>

#include <charconv>
#include <optional>

namespace raikiri::venues::binance {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means binance changed a filter, so the message is dropped
// rather than rounded — rounding is how an order ends up one tick away from
// the intended price.
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
FixedE9 to_e9(std::string_view text) {
    if (text.empty()) {
        return FixedE9::from_raw(0);
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? *value : FixedE9::from_raw(0);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
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
    out.ts_event = ms_to_unix(wire.transaction_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireAggTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
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
    // The flip: `m` says the BUYER was the maker, so the aggressor sold.
    out.aggressor = wire.buyer_is_maker ? Side::Sell : Side::Buy;
    out.ts_event = ms_to_unix(wire.trade_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    if (wire.funding_rate.empty()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = to_e9(wire.funding_rate);
    if (const auto mark = parse_decimal_e9(wire.mark_price);
        mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    out.interval_s = std::visit(
        [](const auto& concrete) -> std::uint32_t {
            if constexpr (requires { concrete.funding_interval_s; }) {
                return concrete.funding_interval_s;
            } else {
                return 0;
            }
        },
        *instrument);
    out.ts_event = ms_to_unix(wire.event_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    // USD-M FUTURES ONLY. The rows come from /fapi/v1/exchangeInfo and the
    // whole adapter speaks to fapi.binance.com; binance spot is another host
    // and another API. Admitting a Spot id here would intern every futures
    // row — perpetuals AND dated quarterlies — as a SpotInstrument with a
    // pinned contract_value, which is worse than refusing because it looks
    // like it worked.
    switch (market) {
    case MarketType::Perp:
        break;
    case MarketType::Spot:
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    }
    if (wire.status != "TRADING") {
        return std::nullopt; // SETTLING, PENDING_TRADING, CLOSE: not tradable now
    }
    if (wire.contract_type != "PERPETUAL") {
        return std::nullopt;
    }

    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.step_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Binance, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = to_e9(wire.min_notional);
    base.contract_value = *parse_decimal_e9("1"); // USD-M perps quote in base units
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, /*funding_interval_s=*/28'800};
}

namespace {

// Signed quantities arrive as decimal text with a leading '-'; the sign has to
// survive into the lot conversion, which divides a signed value by a positive
// step.
[[nodiscard]] std::optional<std::int64_t> signed_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

[[nodiscard]] Side side_from(std::string_view text) noexcept {
    return text == "SELL" ? Side::Sell : Side::Buy;
}

[[nodiscard]] bool copy_id(std::string_view text, ExchangeOrderId& out) {
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

[[nodiscard]] bool copy_coid(std::string_view text, ExchangeClientOrderId& out) {
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

[[nodiscard]] std::optional<OrderEventType> event_for(std::string_view exec_type) noexcept {
    if (exec_type == "NEW") {
        return OrderEventType::Accepted;
    }
    if (exec_type == "TRADE") {
        return OrderEventType::Filled;
    }
    if (exec_type == "CANCELED") {
        return OrderEventType::Canceled;
    }
    if (exec_type == "EXPIRED") {
        return OrderEventType::Expired;
    }
    if (exec_type == "AMENDMENT") {
        return OrderEventType::Updated;
    }
    return std::nullopt;
}

[[nodiscard]] OrderStatus status_from(std::string_view text) noexcept {
    if (text == "FILLED") {
        return OrderStatus::Filled;
    }
    if (text == "PARTIALLY_FILLED") {
        return OrderStatus::PartiallyFilled;
    }
    if (text == "CANCELED") {
        return OrderStatus::Canceled;
    }
    if (text == "EXPIRED") {
        return OrderStatus::Expired;
    }
    if (text == "REJECTED") {
        return OrderStatus::Rejected;
    }
    return OrderStatus::Accepted;
}

} // namespace

std::optional<OrderEvent> map_order_event(const WireOrderUpdate& wire,
                                          const exec::InstrumentTable& table,
                                          UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto type = event_for(wire.exec_type);
    if (!type.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    OrderEvent out{};
    out.type = *type;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.order_type = wire.order_type == "MARKET" ? OrderType::Market : OrderType::Limit;
    out.tif = wire.time_in_force == "IOC"   ? TimeInForce::Ioc
              : wire.time_in_force == "FOK" ? TimeInForce::Fok
                                            : TimeInForce::Gtc;
    if (wire.time_in_force == "GTX") {
        out.flags = OrderFlags::PostOnly;
    }
    if (wire.reduce_only) {
        out.flags = out.flags | OrderFlags::ReduceOnly;
    }
    if (!copy_coid(wire.client_order_id, out.exchange_client_order_id)) {
        return std::nullopt;
    }
    std::array<char, 24> id_text{};
    if (const auto [stop, ec] =
            std::to_chars(id_text.data(), id_text.data() + id_text.size(), wire.order_id);
        ec == std::errc{}) {
        if (!copy_id({id_text.data(), static_cast<std::size_t>(stop - id_text.data())},
                     out.exchange_order_id)) {
            return std::nullopt;
        }
    }

    if (*type == OrderEventType::Filled) {
        const auto price = exact_steps(wire.last_filled_price, base.tick_size);
        const auto qty = exact_steps(wire.last_filled_qty, base.lot_size);
        const auto cum = exact_steps(wire.cum_filled_qty, base.lot_size);
        const auto total = exact_steps(wire.order_qty, base.lot_size);
        if (!price.has_value() || !qty.has_value()) {
            return std::nullopt;
        }
        out.price = PriceTicks::from_raw(*price);
        out.qty = QtyLots::from_raw(*qty);
        out.leaves_qty =
            QtyLots::from_raw(cum.has_value() && total.has_value() ? *total - *cum : 0);
        out.fee = to_e9(wire.commission);
        out.liquidity = wire.is_maker ? Liquidity::Maker : Liquidity::Taker;
        std::array<char, 24> exec_text{};
        if (const auto [stop, ec] =
                std::to_chars(exec_text.data(), exec_text.data() + exec_text.size(), wire.trade_id);
            ec == std::errc{}) {
            if (const auto id = ExecId::from(
                    {exec_text.data(), static_cast<std::size_t>(stop - exec_text.data())});
                id.has_value()) {
                out.exec_id = *id;
            }
        }
    } else {
        const auto price = exact_steps(wire.order_price, base.tick_size);
        const auto qty = exact_steps(wire.order_qty, base.lot_size);
        const auto cum = exact_steps(wire.cum_filled_qty, base.lot_size);
        out.price = PriceTicks::from_raw(price.value_or(0));
        out.qty = QtyLots::from_raw(qty.value_or(0));
        out.leaves_qty =
            QtyLots::from_raw(qty.has_value() && cum.has_value() ? *qty - *cum : qty.value_or(0));
    }

    out.ts_event = ms_to_unix(wire.trade_ms != 0 ? wire.trade_ms : wire.event_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.status = status_from(wire.status);
    if (!copy_coid(wire.client_order_id, out.exchange_client_order_id)) {
        return std::nullopt;
    }
    std::array<char, 24> id_text{};
    if (const auto [stop, ec] =
            std::to_chars(id_text.data(), id_text.data() + id_text.size(), wire.order_id);
        ec == std::errc{}) {
        if (!copy_id({id_text.data(), static_cast<std::size_t>(stop - id_text.data())},
                     out.exchange_order_id)) {
            return std::nullopt;
        }
    }
    const auto price = exact_steps(wire.price, base.tick_size);
    const auto qty = exact_steps(wire.orig_qty, base.lot_size);
    const auto executed = exact_steps(wire.executed_qty, base.lot_size);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    out.price = PriceTicks::from_raw(price.value_or(0));
    out.qty = QtyLots::from_raw(*qty);
    out.leaves = QtyLots::from_raw(executed.has_value() ? *qty - *executed : *qty);
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<exec::FillReport> map_user_trade(const WireTrade& wire,
                                               const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = exact_steps(wire.price, base.tick_size);
    const auto qty = exact_steps(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    exec::FillReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.fee = to_e9(wire.commission);
    out.liquidity = wire.maker ? Liquidity::Maker : Liquidity::Taker;
    std::array<char, 24> text{};
    if (const auto [stop, ec] =
            std::to_chars(text.data(), text.data() + text.size(), wire.trade_id);
        ec == std::errc{}) {
        if (const auto id =
                ExecId::from({text.data(), static_cast<std::size_t>(stop - text.data())});
            id.has_value()) {
            out.exec_id = *id;
        }
    }
    std::array<char, 24> order_text{};
    if (const auto [stop, ec] =
            std::to_chars(order_text.data(), order_text.data() + order_text.size(), wire.order_id);
        ec == std::errc{}) {
        if (!copy_id({order_text.data(), static_cast<std::size_t>(stop - order_text.data())},
                     out.exchange_order_id)) {
            return std::nullopt;
        }
    }
    out.ts_venue = ms_to_unix(wire.time_ms, UnixNanos{0});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePositionRisk& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = signed_steps(wire.position_amt, base.lot_size);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = QtyLots::from_raw(*qty);
    if (const auto entry = parse_decimal_e9(wire.entry_price);
        entry.has_value() && base.tick_size.raw > 0) {
        out.avg_entry = PriceTicks::from_raw(entry->raw / base.tick_size.raw);
    }
    return out;
}

} // namespace raikiri::venues::binance
