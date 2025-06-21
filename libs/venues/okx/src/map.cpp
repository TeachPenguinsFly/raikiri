#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/okx/map.hpp>

#include <optional>

namespace raikiri::venues::okx {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means OKX changed an instrument's precision, so the message is
// dropped rather than rounded — rounding is how an order ends up one tick away
// from the intended price.
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
// A funding rate is a ratio, not money and not a price grid, and OKX pads it
// to sixteen fractional digits. An exact reader refuses every one of them —
// which it did, silently, for as long as this called parse_decimal_e9: the
// channel was subscribed, the frames arrived, and not one rate reached shm.
// parse_rate_e9 is exact through 1e-9 and truncates the dust below.
std::optional<FixedE9> rate_of(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    return parse_rate_e9(text);
}

// A fee is money, so it is read exactly wherever it can be — but OKX pads
// inverse-contract fees past 1e-9 of a coin, and an exact reader refuses
// every one of those. parse_rate_e9 is exact for anything with nine or fewer
// fractional digits and truncates the dust below that toward zero, which can
// only ever under-report our own cost by less than a nanocoin. Never reach
// for it on a price or a size.
//
// The sign is flipped on the way in: OKX writes a fee CHARGED as negative and
// a rebate as positive, while the ledger this feeds — and binance and bybit
// before it — count a positive number as money leaving.
FixedE9 fee_of(std::string_view text) {
    if (text.empty()) {
        return FixedE9::from_raw(0);
    }
    const auto value = parse_rate_e9(text);
    return value.has_value() ? FixedE9::from_raw(-value->raw) : FixedE9::from_raw(0);
}

Liquidity liquidity_from(std::string_view exec_type) noexcept {
    if (exec_type == "M") {
        return Liquidity::Maker;
    }
    if (exec_type == "T") {
        return Liquidity::Taker;
    }
    return Liquidity::Unknown;
}

// OKX publishes no remaining quantity: it ships the order's total size and
// the cumulative fill, and the difference is the remainder. An order whose
// two halves do not both convert exactly yields nothing rather than a leaves
// of zero, which downstream reads as "this order is done".
std::optional<QtyLots> leaves_of(const WireOrder& wire, const InstrumentBase& base) {
    const auto total = to_lots(wire.sz, base);
    if (!total.has_value()) {
        return std::nullopt;
    }
    if (wire.acc_fill_sz.empty()) {
        return *total;
    }
    const auto filled = to_lots(wire.acc_fill_sz, base);
    if (!filled.has_value() || filled->raw > total->raw) {
        return std::nullopt;
    }
    return QtyLots::from_raw(total->raw - filled->raw);
}

bool copy_order_id(std::string_view text, ExchangeOrderId& out) {
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

// Our own ULID coming back from the venue. A value that does not fit the
// field cannot be one of ours, so the event stays unattributed rather than
// being matched to a truncated lookalike.
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

// ctMult is a plain count, not a decimal quantity; anything fractional would
// make the contract size a non-integer multiple of ctVal, which OKX does not
// define, so the row is refused instead of approximated.
std::optional<std::int64_t> whole_number(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw <= 0 || value->raw % k_e9 != 0) {
        return std::nullopt;
    }
    return value->raw / k_e9;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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

std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = rate_of(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base_of(*instrument).id;
    out.rate = *rate;
    if (const auto predicted = rate_of(wire.next_funding_rate); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    out.next_funding_time = ms_to_unix(wire.funding_ms, UnixNanos{0});
    if (wire.next_funding_ms > wire.funding_ms && wire.funding_ms != 0) {
        out.interval_s =
            static_cast<std::uint32_t>((wire.next_funding_ms - wire.funding_ms) / 1000);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

Side side_from(std::string_view text) noexcept {
    return text == "sell" ? Side::Sell : Side::Buy;
}

std::optional<OrderEvent>
map_order_fill(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.trade_id.empty()) {
        return std::nullopt; // no trade in this update; see map.hpp
    }
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.fill_px, base);
    const auto qty = to_lots(wire.fill_sz, base);
    // A Filled event whose remainder cannot be computed is worse than no
    // event: downstream reads leaves_qty against the order's previous
    // remainder to size the fill, and a fabricated zero closes the order at
    // the wrong quantity. Dropping it leaves the reconciliation pulls to
    // correct a live order, which they can.
    const auto leaves = leaves_of(wire, base);
    if (!price.has_value() || !qty.has_value() || !leaves.has_value()) {
        return std::nullopt;
    }
    const auto exec_id = ExecId::from(wire.trade_id);
    if (!exec_id.has_value()) {
        return std::nullopt; // a dedupe key we cannot store is not usable
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.liquidity = liquidity_from(wire.exec_type);
    if (!copy_client_id(wire.cl_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.ord_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.exec_id = *exec_id;
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = *leaves;
    out.fee = fee_of(wire.fill_fee);
    out.ts_event = ms_to_unix(wire.fill_ms != 0 ? wire.fill_ms : wire.u_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    OrderEventType type{};
    if (wire.amend_result == "-1") {
        // The order is unchanged and still working; a Rejected here would
        // close an order the venue still holds.
        type = OrderEventType::ModifyRejected;
    } else if (wire.code != k_ok) {
        type = OrderEventType::Rejected;
    } else {
        switch (wire.state) {
        case WireOrderState::Live:
            type = OrderEventType::Accepted;
            break;
        case WireOrderState::Canceled:
        case WireOrderState::MmpCanceled:
            // cancelSource 13 is an FOK that could not fill whole and 14 an
            // IOC remainder. The venue spells both "canceled"; the domain
            // distinguishes an expiry from a withdrawal, and a strategy
            // counting its own cancels should not see the venue's.
            type = (wire.cancel_source == "13" || wire.cancel_source == "14")
                       ? OrderEventType::Expired
                       : OrderEventType::Canceled;
            break;
        case WireOrderState::PartiallyFilled:
        case WireOrderState::Filled:
        case WireOrderState::Unknown:
            return std::nullopt; // the fill half of this message carried it
        }
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.order_type = wire.ord_type == "market" ? OrderType::Market : OrderType::Limit;
    if (!copy_client_id(wire.cl_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.ord_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.px, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.sz, base); qty.has_value()) {
        out.qty = *qty;
    }
    if (const auto leaves = leaves_of(wire, base); leaves.has_value()) {
        out.leaves_qty = *leaves;
    }
    out.ts_event = ms_to_unix(wire.u_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    exec::OrderStatusReport out{};
    switch (wire.state) {
    case WireOrderState::Live:
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderState::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    case WireOrderState::Filled:
    case WireOrderState::Canceled:
    case WireOrderState::MmpCanceled:
    case WireOrderState::Unknown:
        return std::nullopt; // orders-pending is defined to return neither
    }
    out.instrument = base.id;
    out.side = side_from(wire.side);
    if (!copy_client_id(wire.cl_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.ord_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.px, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.sz, base); qty.has_value()) {
        out.qty = *qty;
    }
    if (const auto leaves = leaves_of(wire, base); leaves.has_value()) {
        out.leaves = *leaves;
    }
    out.ts_venue = ms_to_unix(wire.u_ms, UnixNanos{});
    return out;
}

std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.fill_px, base);
    const auto qty = to_lots(wire.fill_sz, base);
    const auto exec_id = ExecId::from(wire.trade_id);
    if (!price.has_value() || !qty.has_value() || !exec_id.has_value()) {
        return std::nullopt;
    }

    exec::FillReport out{};
    out.exec_id = *exec_id;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    if (!copy_client_id(wire.cl_ord_id, out.exchange_client_order_id) ||
        !copy_order_id(wire.ord_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.fee = fee_of(wire.fee);
    out.liquidity = liquidity_from(wire.exec_type);
    out.ts_venue = ms_to_unix(wire.fill_ms, UnixNanos{});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto size = to_lots(wire.pos, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    // Net mode signs `pos` itself; long/short mode returns one unsigned row a
    // side. Reading a short row as written is a position error of twice the
    // size, in the direction that liquidates.
    out.net_qty = wire.pos_side == "short"
                      ? QtyLots::from_raw(size->raw < 0 ? size->raw : -size->raw)
                      : *size;
    if (const auto avg = to_ticks(wire.avg_px, base); avg.has_value()) {
        out.avg_entry = *avg;
    }
    out.ts_venue = ms_to_unix(wire.u_ms, UnixNanos{});
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (wire.state != "live") {
        return std::nullopt;
    }
    if (market == MarketType::Spot) {
        if (wire.inst_type != "SPOT") {
            return std::nullopt;
        }
    } else if (market == MarketType::Perp) {
        if (wire.inst_type != "SWAP") {
            return std::nullopt;
        }
    } else {
        return std::nullopt; // futures and options need expiry/strike first
    }

    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.lot_size);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.inst_id);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Okx, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (market == MarketType::Spot) {
        base.contract_value = *parse_decimal_e9("1");
        base.kind = ContractKind::Linear;
        return SpotInstrument{base};
    }
    const auto ct_val = parse_decimal_e9(wire.ct_val);
    const auto ct_mult = whole_number(wire.ct_mult);
    if (!ct_val.has_value() || ct_val->raw <= 0 || !ct_mult.has_value()) {
        return std::nullopt;
    }
    // BTC-USDT-SWAP and BTC-USD-SWAP differ ONLY here: ctVal is BTC on the
    // first and USD on the second, and the numbers look equally plausible.
    // A row that will not say which is refused rather than assumed linear.
    if (wire.ct_type == "linear") {
        base.kind = ContractKind::Linear;
    } else if (wire.ct_type == "inverse") {
        base.kind = ContractKind::Inverse;
    } else {
        return std::nullopt;
    }
    base.contract_value = ct_val->scaled_by(*ct_mult);
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::okx
