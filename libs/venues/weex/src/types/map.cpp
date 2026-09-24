#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/weex/map.hpp>

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace raikiri::venues::weex {

namespace {

[[nodiscard]] AssetClass asset_class_of(std::string_view underlying_type) noexcept {
    if (underlying_type == "COIN") {
        return AssetClass::Crypto;
    }
    if (underlying_type == "Stocks" || underlying_type == "Pre-IPO") {
        return AssetClass::Equity;
    }
    if (underlying_type == "Indices") {
        return AssetClass::Index;
    }
    if (underlying_type == "Metals" || underlying_type == "Commodities") {
        return AssetClass::Commodity;
    }
    if (underlying_type == "Forex") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

// exact conversion against the instrument's step; inexact values are dropped
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return book::exact_steps(*value, step);
}

std::optional<PriceTicks> to_ticks(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view text, const InstrumentBase& base) {
    const auto steps = exact_steps(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

// nullopt past 1e-9, a step FixedE9 cannot hold
std::optional<FixedE9> step_from_precision(std::uint64_t precision) noexcept {
    if (precision > 9) {
        return std::nullopt;
    }
    std::int64_t raw = 1;
    for (std::uint64_t i = precision; i < 9; ++i) {
        raw *= 10;
    }
    return FixedE9::from_raw(raw);
}

// nullopt for a count that does not divide the day
std::optional<std::uint32_t> interval_from(std::uint32_t settlements_per_day) noexcept {
    constexpr std::uint32_t k_seconds_per_day = 86'400;
    if (settlements_per_day == 0 || k_seconds_per_day % settlements_per_day != 0) {
        return std::nullopt;
    }
    return k_seconds_per_day / settlements_per_day;
}

// weex has no PARTIALLY_FILLED, so the quantities decide; unknown words fall through to the same arithmetic
OrderStatus
status_of(WireOrderStatus status, QtyLots qty, std::optional<QtyLots> executed) noexcept {
    switch (status) {
    case WireOrderStatus::Filled:
        return OrderStatus::Filled;
    case WireOrderStatus::Canceled:
        return OrderStatus::Canceled;
    case WireOrderStatus::Canceling:
        return OrderStatus::PendingCancel;
    case WireOrderStatus::Untriggered:
    case WireOrderStatus::Unactivated:
    case WireOrderStatus::Pending:
    case WireOrderStatus::New:
    case WireOrderStatus::Unknown:
        break;
    }
    if (!executed.has_value()) {
        return OrderStatus::Accepted;
    }
    if (executed->raw >= qty.raw) {
        return OrderStatus::Filled; // nothing is left to work
    }
    return executed->raw > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<TradeTick> map_trade(const WirePrint& wire,
                                   std::string_view symbol,
                                   const exec::InstrumentTable& table,
                                   UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
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
    // m true means the buyer was maker, so the aggressor sold
    out.aggressor = wire.buyer_is_maker ? Side::Sell : Side::Buy;
    // the print's T; one frame carries several prints
    out.ts_event = ms_to_unix(wire.trade_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.volume.empty() && wire.quote_volume.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    // turnover is an amount, read with truncation; an omitted column stays undef
    if (const auto base_volume = parse_amount_e9(wire.volume); base_volume.has_value()) {
        out.volume_24h_base = *base_volume;
    }
    if (const auto quote_volume = parse_turnover_e6(wire.quote_volume); quote_volume.has_value()) {
        out.volume_24h_quote = *quote_volume;
    }
    out.ts_event = ms_to_unix(wire.close_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WirePremium& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    if (wire.funding_rate.empty()) {
        return std::nullopt; // quotes no funding: not the same as funding at zero
    }
    // truncating: weex pads rates past 1e-9
    const auto rate = parse_rate_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto forecast = parse_rate_e9(wire.forecast_rate); forecast.has_value()) {
        out.predicted_rate = *forecast; // absent leaves it undef
    }
    // exact or undef; rounding would invent a price
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
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

Side side_from(std::string_view text) noexcept {
    return text == "SELL" ? Side::Sell : Side::Buy;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = to_lots(wire.orig_qty, base);
    if (!qty.has_value()) {
        return std::nullopt; // an order whose size will not convert is unreportable
    }

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.qty = *qty;
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price; // a market order prices at "0", which converts to zero ticks
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    // ids longer than ours come from other clients and stay unset
    if (const auto ulid = ExchangeClientOrderId::from(wire.client_order_id); ulid.has_value()) {
        out.exchange_client_order_id = *ulid;
    }
    // cumulative; an unreadable figure leaves leaves undef
    const auto executed = to_lots(wire.executed_qty, base);
    if (executed.has_value()) {
        out.leaves = QtyLots::from_raw(qty->raw - executed->raw);
    }
    out.status = status_of(wire.status, *qty, executed);
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table) {
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

    exec::FillReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.price = *price;
    out.qty = *qty;
    if (const auto id = ExecId::from(wire.fill_id); id.has_value()) {
        out.exec_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    // commission is an amount, read with truncation; commissionAsset is dropped and assumed quote
    if (const auto fee = parse_amount_e9(wire.commission); fee.has_value()) {
        out.fee = *fee;
    }
    out.liquidity = wire.maker ? Liquidity::Maker : Liquidity::Taker;
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
    // size is unsigned; anything but LONG reads as short
    out.net_qty = wire.side == "LONG" ? *size : QtyLots::from_raw(-size->raw);
    // avg_entry stays undef; weex states no entry price
    out.ts_venue = ms_to_unix(wire.updated_ms, UnixNanos{0});
    return out;
}

std::vector<exec::PositionReport> map_positions(std::span<const WirePosition> rows,
                                                const exec::InstrumentTable& table) {
    std::vector<exec::PositionReport> out;
    out.reserve(rows.size());
    std::unordered_map<std::uint64_t, std::size_t> seen;
    for (const auto& wire : rows) {
        const auto report = map_position(wire, table);
        if (!report.has_value()) {
            continue;
        }
        const auto found = seen.find(report->instrument.packed);
        if (found == seen.end()) {
            seen.emplace(report->instrument.packed, out.size());
            out.push_back(*report);
            continue;
        }
        auto& held = out[found->second];
        held.net_qty = QtyLots::from_raw(held.net_qty.raw + report->net_qty.raw);
        if (report->ts_venue.ns > held.ts_venue.ns) {
            held.ts_venue = report->ts_venue;
        }
    }
    return out;
}

std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos ts_venue) {
    const auto asset = to_asset_code(wire.asset);
    if (!asset.has_value()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    // truncating: balances carry digits past 1e-9; an omitted column stays undef
    if (const auto total = parse_amount_e9(wire.balance); total.has_value()) {
        out.total = *total;
    }
    if (const auto available = parse_amount_e9(wire.available_balance); available.has_value()) {
        out.available = *available;
    }
    if (const auto unrealized = parse_amount_e9(wire.unrealize_pnl); unrealized.has_value()) {
        out.unrealized = *unrealized;
    }
    out.ts_venue = ts_venue;
    return out;
}

std::optional<LedgerEntry>
map_funding_charge(const WireBill& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!is_funding_bill(wire)) {
        return std::nullopt; // a deposit booked as funding is money invented
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt; // a contract this gateway does not carry
    }
    const auto amount = parse_amount_e9(wire.income);
    if (!amount.has_value()) {
        return std::nullopt; // an unreadable figure is not a charge of zero
    }

    LedgerEntry out{};
    out.instrument = base_of(*instrument).id;
    // already signed as the account moved
    out.amount = *amount;
    out.kind = LedgerKind::Funding;
    out.settled_at = UnixNanos{wire.time_ms * 1'000'000ULL};
    // one id per bill, so a settlement is booked once
    if (const auto id = ExecId::from(wire.bill_id); id.has_value()) {
        out.reference = *id;
    }
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    // the contract tree lists only usdt perpetuals
    switch (market) {
    case MarketType::Perp:
        break;
    case MarketType::Spot:
    case MarketType::Future:
    case MarketType::Option:
        return std::nullopt;
    }
    // TRADIFI_PERPETUAL settles and funds like PERPETUAL
    if (wire.contract_type != "PERPETUAL" && wire.contract_type != "TRADIFI_PERPETUAL") {
        return std::nullopt;
    }

    const auto tick = step_from_precision(wire.price_precision);
    const auto lot = parse_decimal_e9(wire.contract_val);
    if (!tick.has_value() || !lot.has_value() || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto interval = interval_from(wire.settlements_per_day);
    if (!interval.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }
    // rows without a stated pair are dropped so they never show as a market of their own
    const auto canonical = canonical_symbol(wire.base_asset, wire.quote_asset);
    if (!canonical.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional; InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Weex, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *canonical;
    base.asset_class = asset_class_of(wire.underlying_type);
    base.tick_size = *tick;
    base.lot_size = *lot;
    // sizes are base amounts; contractVal is the lot size
    base.contract_value = FixedE9::from_raw(1'000'000'000);
    base.kind = ContractKind::Linear;
    // min_notional and fees stay undef; weex states no minimum and only public-tier fees
    return PerpInstrument{base, *interval};
}

} // namespace raikiri::venues::weex
