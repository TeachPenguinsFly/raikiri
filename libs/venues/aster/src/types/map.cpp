#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/aster/map.hpp>

#include <charconv>
#include <optional>
#include <string>

namespace raikiri::venues::aster {

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

// rates keep 1e9 scale, not instrument ticks
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

std::optional<Bbo> map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ms_to_unix(wire.transaction_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    // m means the buyer was the maker, so the aggressor sold
    out.aggressor = wire.buyer_is_maker ? Side::Sell : Side::Buy;
    out.ts_event = ms_to_unix(wire.trade_ms, UnixNanos{});
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
    if (const auto mark = parse_decimal_e9(wire.mark_price); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    if (const auto index = parse_amount_e9(wire.index_price); index.has_value() && base.tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(index->raw / base.tick_size.raw);
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
    out.ts_event = ms_to_unix(wire.event_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

namespace {

// underlyingType says COIN on every row; the sub-type and symbolType tell a stock from a coin
[[nodiscard]] AssetClass asset_class_of(const WireInstrument& wire) noexcept {
    const auto has = [&](std::string_view tag) {
        return wire.underlying_sub_type.find(std::string{"\""} + std::string{tag} + "\"") != std::string_view::npos;
    };
    if (has("STOCK") || has("ETF")) {
        return AssetClass::Equity;
    }
    if (has("Commodities")) {
        return AssetClass::Commodity;
    }
    return wire.symbol_type == 1 ? AssetClass::Unknown : AssetClass::Crypto;
}

} // namespace

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    switch (market) {
    case MarketType::Perp:
    case MarketType::Spot:
        break;
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    }
    if (wire.status != "TRADING") {
        return std::nullopt; // SETTLING, PENDING_TRADING, CLOSE: not tradable now
    }
    // spot rows name no contract type, futures rows always do
    if (wire.contract_type != (market == MarketType::Spot ? "" : "PERPETUAL")) {
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
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Aster, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    // rows without a stated pair are dropped
    const auto canonical = canonical_symbol(wire.base_asset, wire.quote_asset);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = to_e9(wire.min_notional);
    base.contract_value = *parse_decimal_e9("1"); // USD-M perps quote in base units
    base.kind = ContractKind::Linear;
    base.asset_class = asset_class_of(wire);
    if (market == MarketType::Spot) {
        return SpotInstrument{base};
    }
    // interval stamped from fundingInfo by the catalogue loader; 0 means unstated
    return PerpInstrument{base, wire.funding_interval_s};
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker24h& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.volume.empty() && wire.quote_volume.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = to_e9(wire.volume);
    out.volume_24h_quote = parse_turnover_e6(wire.quote_volume).value_or(FixedE6::undef());
    out.ts_event = ms_to_unix(wire.close_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

namespace {

// the sign survives into the lot conversion
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

std::optional<OrderEvent>
map_order_event(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    if (const auto [stop, ec] = std::to_chars(id_text.data(), id_text.data() + id_text.size(), wire.order_id);
        ec == std::errc{}) {
        if (!copy_id({id_text.data(), static_cast<std::size_t>(stop - id_text.data())}, out.exchange_order_id)) {
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
        out.leaves_qty = QtyLots::from_raw(cum.has_value() && total.has_value() ? *total - *cum : 0);
        out.fee = to_e9(wire.commission);
        out.liquidity = wire.is_maker ? Liquidity::Maker : Liquidity::Taker;
        std::array<char, 24> exec_text{};
        if (const auto [stop, ec] = std::to_chars(exec_text.data(), exec_text.data() + exec_text.size(), wire.trade_id);
            ec == std::errc{}) {
            if (const auto id = ExecId::from({exec_text.data(), static_cast<std::size_t>(stop - exec_text.data())});
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
        out.leaves_qty = QtyLots::from_raw(qty.has_value() && cum.has_value() ? *qty - *cum : qty.value_or(0));
    }

    out.ts_event = ms_to_unix(wire.trade_ms != 0 ? wire.trade_ms : wire.event_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire, const exec::InstrumentTable& table) {
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
    if (const auto [stop, ec] = std::to_chars(id_text.data(), id_text.data() + id_text.size(), wire.order_id);
        ec == std::errc{}) {
        if (!copy_id({id_text.data(), static_cast<std::size_t>(stop - id_text.data())}, out.exchange_order_id)) {
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

std::optional<exec::FillReport> map_user_trade(const WireTrade& wire, const exec::InstrumentTable& table) {
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
    if (const auto [stop, ec] = std::to_chars(text.data(), text.data() + text.size(), wire.trade_id);
        ec == std::errc{}) {
        if (const auto id = ExecId::from({text.data(), static_cast<std::size_t>(stop - text.data())}); id.has_value()) {
            out.exec_id = *id;
        }
    }
    std::array<char, 24> order_text{};
    if (const auto [stop, ec] = std::to_chars(order_text.data(), order_text.data() + order_text.size(), wire.order_id);
        ec == std::errc{}) {
        if (!copy_id({order_text.data(), static_cast<std::size_t>(stop - order_text.data())}, out.exchange_order_id)) {
            return std::nullopt;
        }
    }
    out.ts_venue = ms_to_unix(wire.time_ms, UnixNanos{0});
    return out;
}

namespace {

// a wallet figure, truncated so an extra digit does not fail the read
[[nodiscard]] std::optional<FixedE9> to_amount(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

[[nodiscard]] bool has_value_other_than_zero(const std::optional<FixedE9>& value) noexcept {
    return value.has_value() && value->raw != 0;
}

} // namespace

std::optional<BalanceReport> map_balance(const WireAssetBalance& wire, bool multi_assets_margin) {
    const auto asset = to_asset_code(wire.asset);
    if (!asset.has_value() || asset->empty()) {
        return std::nullopt;
    }
    const auto total = to_amount(wire.wallet_balance);
    const auto unrealized = to_amount(wire.unrealized_profit);
    // drops rows aster reported empty with no pnl; an unsent wallet figure is kept
    const bool wallet_reported_empty = total.has_value() && total->raw == 0;
    if (wallet_reported_empty && !has_value_other_than_zero(unrealized)) {
        return std::nullopt;
    }

    BalanceReport out{};
    out.asset = *asset;
    // each field stays undef where aster sent nothing readable
    if (total.has_value()) {
        out.total = *total;
    }
    if (unrealized.has_value()) {
        out.unrealized = *unrealized;
    }
    // in multi-assets mode aster repeats one account-wide figure on every row, so it stays undef there
    if (!multi_assets_margin) {
        if (const auto available = to_amount(wire.available_balance); available.has_value()) {
            out.available = *available;
        }
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePositionRisk& wire, const exec::InstrumentTable& table) {
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
    if (const auto entry = parse_decimal_e9(wire.entry_price); entry.has_value() && base.tick_size.raw > 0) {
        out.avg_entry = PriceTicks::from_raw(entry->raw / base.tick_size.raw);
    }
    return out;
}

} // namespace raikiri::venues::aster
