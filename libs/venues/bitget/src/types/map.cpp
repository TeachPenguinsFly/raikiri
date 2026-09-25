#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/bitget/map.hpp>

#include <optional>
#include <variant>

namespace raikiri::venues::bitget {

namespace {

// a value off the step means the venue changed precision, so it is dropped
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

// rates stay at 1e9 scale
std::optional<FixedE9> to_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? std::optional{*value} : std::nullopt;
}

// turnover truncates below 1e-9 rather than refusing the figure
std::optional<FixedE9> turnover_of(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

// truncates below 1e-9 so a padded figure still books
std::optional<FixedE9> settlement_of(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

// zero is a valid pricePlace
std::optional<std::int64_t> whole_number(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw < 0 || value->raw % k_e9 != 0) {
        return std::nullopt;
    }
    return value->raw / k_e9;
}

// tick is priceEndStep * 10^-pricePlace; a place past nine gives a zero step and is refused
std::optional<FixedE9> decimal_unit(std::string_view place_text) {
    const auto place = whole_number(place_text);
    if (!place.has_value() || *place > 9) {
        return std::nullopt;
    }
    std::int64_t unit = k_e9;
    for (std::int64_t i = 0; i < *place; ++i) {
        unit /= 10;
    }
    return FixedE9::from_raw(unit);
}

std::optional<FixedE9> tick_size_of(std::string_view place_text, std::string_view end_step_text) {
    const auto unit = decimal_unit(place_text);
    const auto end_step = whole_number(end_step_text);
    if (!unit.has_value() || !end_step.has_value() || *end_step <= 0) {
        return std::nullopt;
    }
    std::int64_t raw = 0;
    if (__builtin_mul_overflow(unit->raw, *end_step, &raw)) {
        return std::nullopt;
    }
    return FixedE9::from_raw(raw);
}

// a value too long for the field cannot be ours, so the event stays unattributed
bool copy_client_id(std::string_view text, ExchangeClientOrderId& out) {
    if (text.empty()) {
        return true; // absent is fine; wrong is not
    }
    const auto id = ExchangeClientOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

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

// tradeScope is T/M on the orders channel and taker/maker on the fills endpoints
Liquidity liquidity_from(std::string_view scope) noexcept {
    if (scope == "M" || scope == "maker") {
        return Liquidity::Maker;
    }
    if (scope == "T" || scope == "taker") {
        return Liquidity::Taker;
    }
    return Liquidity::Unknown;
}

TimeInForce tif_from(std::string_view force) noexcept {
    if (force == "ioc") {
        return TimeInForce::Ioc;
    }
    if (force == "fok") {
        return TimeInForce::Fok;
    }
    return TimeInForce::Gtc; // gtc and post_only both rest on the book
}

// bitget signs a fee charge negative; flipped once here
FixedE9 charge_from(std::string_view text) {
    if (text.empty()) {
        return FixedE9::from_raw(0);
    }
    const auto value = parse_number_e9(text);
    return value.has_value() ? FixedE9::from_raw(-value->raw) : FixedE9::from_raw(0);
}

// nullopt where bitget states size in the quote coin (spot market buys)
std::optional<QtyLots> order_qty(const WireOrderUpdate& wire, const InstrumentBase& base) {
    if (base.id.market() == MarketType::Spot && wire.order_type == "market" && wire.side == "buy") {
        return std::nullopt;
    }
    return to_lots(wire.new_size.empty() ? wire.size : wire.new_size, base);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo> map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
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

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }
    const auto& base = base_of(*instrument);

    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_funding_ms, UnixNanos{0});
    if (const auto* perp = std::get_if<PerpInstrument>(instrument); perp != nullptr) {
        out.interval_s = perp->funding_interval_s;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // both columns missing means no turnover update, not zero
    if (wire.volume_24h_base.empty() && wire.volume_24h_quote.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = turnover_of(wire.volume_24h_base).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = parse_turnover_e6(wire.volume_24h_quote).value_or(FixedE6::from_raw(0));
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
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

std::optional<BookSnapshot> map_book(const WireBooks& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!wire.snapshot) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    if (wire.seq_id > 0) {
        out.venue_seq = static_cast<std::uint64_t>(wire.seq_id);
    }

    const auto fill = [&](std::span<const WireLevel> src, std::array<BookLevel, k_book_levels>& dst) {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, base);
            const auto qty = to_lots(level.qty, base);
            // an off-step level truncates the book here rather than being rounded
            if (!price.has_value() || !qty.has_value()) {
                break;
            }
            dst[n] = {.price = *price, .qty = *qty};
            ++n;
        }
        return n;
    };
    out.bid_count = fill(wire.bids, out.bids);
    out.ask_count = fill(wire.asks, out.asks);
    if (out.bid_count == 0 && out.ask_count == 0) {
        return std::nullopt;
    }
    return out;
}

Side side_from(std::string_view text) noexcept {
    return text == "sell" ? Side::Sell : Side::Buy;
}

std::optional<OrderEvent>
map_order_update(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt; // an instrument this process never loaded
    }
    const auto& base = base_of(*instrument);

    OrderEvent out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.order_type = wire.order_type == "market" ? OrderType::Market : OrderType::Limit;
    out.tif = tif_from(wire.force);
    if (!copy_client_id(wire.client_oid, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.ts_init = ts_init;

    const auto total = order_qty(wire, base);
    const auto filled = to_lots(wire.acc_base_volume, base);

    if (!wire.trade_id.empty() && !wire.base_volume.empty()) {
        const auto price = to_ticks(wire.fill_price, base);
        const auto qty = to_lots(wire.base_volume, base);
        const auto exec_id = ExecId::from(wire.trade_id);
        if (!price.has_value() || !qty.has_value() || !exec_id.has_value()) {
            return std::nullopt; // a fill we cannot price, size or dedupe is not usable
        }
        out.type = OrderEventType::Filled;
        out.exec_id = *exec_id;
        out.price = *price;
        out.qty = *qty;
        out.liquidity = liquidity_from(wire.trade_scope);
        out.fee = charge_from(wire.fill_fee);
        // bitget publishes no remaining qty; it is size less filled
        out.leaves_qty = wire.status == WireOrderStatus::Filled      ? QtyLots::from_raw(0)
                         : (total.has_value() && filled.has_value()) ? QtyLots::from_raw(total->raw - filled->raw)
                                                                     : QtyLots::from_raw(0);
        out.ts_event = ms_to_unix(wire.fill_ms != 0 ? wire.fill_ms : wire.update_ms, ts_init);
        return out;
    }

    switch (wire.status) {
    case WireOrderStatus::Live:
        out.type = OrderEventType::Accepted;
        break;
    case WireOrderStatus::Canceled:
        out.type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::PartiallyFilled:
    case WireOrderStatus::Filled:
        // the trade reported itself above
        return std::nullopt;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (total.has_value()) {
        out.qty = *total;
    }
    out.leaves_qty = out.type == OrderEventType::Canceled        ? QtyLots::from_raw(0)
                     : (total.has_value() && filled.has_value()) ? QtyLots::from_raw(total->raw - filled->raw)
                                                                 : total.value_or(QtyLots::from_raw(0));
    out.ts_event = ms_to_unix(wire.update_ms, ts_init);
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrderUpdate& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    if (!copy_client_id(wire.client_oid, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    switch (wire.status) {
    case WireOrderStatus::Live:
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderStatus::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    case WireOrderStatus::Filled:
    case WireOrderStatus::Canceled:
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    const auto total = order_qty(wire, base);
    if (total.has_value()) {
        out.qty = *total;
        if (const auto filled = to_lots(wire.acc_base_volume, base); filled.has_value()) {
            out.leaves = QtyLots::from_raw(total->raw - filled->raw);
        } else {
            out.leaves = *total;
        }
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{});
    return out;
}

std::optional<exec::FillReport> map_fill_report(const WireFill& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.size, base);
    const auto exec_id = ExecId::from(wire.trade_id);
    if (!price.has_value() || !qty.has_value() || !exec_id.has_value()) {
        return std::nullopt;
    }

    exec::FillReport out{};
    out.exec_id = *exec_id;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.price = *price;
    out.qty = *qty;
    out.fee = charge_from(wire.fee);
    out.liquidity = liquidity_from(wire.trade_scope);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto size = to_lots(wire.total, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    // total is unsigned; holdSide gives the direction
    out.net_qty = wire.hold_side == "short" ? QtyLots::from_raw(-size->raw) : *size;
    if (const auto avg = to_ticks(wire.open_price_avg, base); avg.has_value()) {
        out.avg_entry = *avg;
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{});
    return out;
}

std::optional<Instrument> map_instrument(const WireSpotSymbol& wire) {
    if (wire.status != "online") {
        return std::nullopt; // halt and offline rows are listed but untradable
    }
    const auto tick = decimal_unit(wire.price_precision);
    const auto lot = decimal_unit(wire.quantity_precision);
    if (!tick.has_value() || !lot.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bitget, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    // a row without a stated pair is dropped so cross-venue grouping stays correct
    const auto canonical = canonical_symbol(wire.base_coin, wire.quote_coin);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min_notional = to_e9(wire.min_trade_usdt); min_notional.has_value()) {
        base.min_notional = *min_notional;
    }
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.symbol_type != "perpetual") {
        return std::nullopt;
    }
    if (wire.symbol_status != "normal") {
        return std::nullopt;
    }

    const auto tick = tick_size_of(wire.price_place, wire.price_end_step);
    const auto lot = to_e9(wire.size_multiplier);
    if (!tick.has_value() || !lot.has_value() || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional; InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Bitget, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    // dropped when the pair is not stated
    const auto canonical = canonical_symbol(wire.base_coin, wire.quote_coin);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min_notional = to_e9(wire.min_trade_usdt); min_notional.has_value()) {
        base.min_notional = *min_notional;
    }
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    std::uint32_t interval_s = 0;
    if (const auto hours = whole_number(wire.fund_interval); hours.has_value()) {
        interval_s = static_cast<std::uint32_t>(*hours * 3600);
    }
    return PerpInstrument{base, interval_s};
}

std::optional<LedgerEntry> map_bill(const WireBill& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.business_type != "contract_settle_fee") {
        // other rows are trades, transfers or margin moves, already booked or not trading money
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    // taken as signed; bitget signs the settlement
    const auto amount = settlement_of(wire.amount);
    if (!amount.has_value()) {
        return std::nullopt; // an unreadable figure is not a zero charge
    }

    LedgerEntry charge{};
    charge.instrument = base_of(*instrument).id;
    charge.amount = *amount;
    charge.kind = LedgerKind::Funding;
    charge.settled_at = ms_to_unix(wire.c_time_ms, UnixNanos{});
    // bitget's per-row id separates settlements with equal stamps
    if (const auto id = ExecId::from(wire.bill_id); id.has_value()) {
        charge.reference = *id;
    }
    charge.ts_init = ts_init;
    return charge;
}

} // namespace raikiri::venues::bitget
