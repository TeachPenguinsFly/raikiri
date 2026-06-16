#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/rest.hpp>

#include <array>
#include <optional>

namespace raikiri::venues::gate {

namespace {

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means Gate changed an instrument's precision, so the message is
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
std::optional<FixedE9> to_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? std::optional{*value} : std::nullopt;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

UnixNanos s_to_unix(std::uint64_t seconds) noexcept {
    return UnixNanos{seconds * 1'000'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
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

namespace {

// Levels stop at the domain's ceiling rather than failing: a book truncated to
// what the consumer can hold is still a correct top-of-book, and the count
// says how much of it is real.
std::uint32_t fill_levels(std::span<const WireLevel> from,
                          const InstrumentBase& base,
                          std::array<BookLevel, k_book_levels>& into) {
    std::uint32_t count = 0;
    for (const auto& level : from) {
        if (count == k_book_levels) {
            break;
        }
        const auto price = to_ticks(level.price, base);
        const auto qty = to_lots(level.qty, base);
        if (!price.has_value() || !qty.has_value()) {
            return 0; // a level that will not convert makes the whole book a guess
        }
        into[count] = BookLevel{.price = *price, .qty = *qty};
        ++count;
    }
    return count;
}

} // namespace

std::optional<BookSnapshot>
map_book(const WireBookUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    BookSnapshot out{};
    out.instrument = base.id;
    out.bid_count = fill_levels(wire.bids, base, out.bids);
    out.ask_count = fill_levels(wire.asks, base, out.asks);
    if (out.bid_count == 0 && out.ask_count == 0) {
        return std::nullopt;
    }
    out.venue_seq = wire.last_update_id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
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
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = s_to_unix(wire.funding_next_apply_s);
    out.interval_s = wire.funding_interval_s;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.volume_24h_base.empty() && wire.volume_24h_quote.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    out.volume_24h_base = to_e9(wire.volume_24h_base).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = parse_turnover_e6(wire.volume_24h_quote).value_or(FixedE6::from_raw(0));
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

namespace {

// Gate names the pair nowhere but the symbol: a futures contract's `name` and a
// spot pair's `id` are both `BTC_USDT`. A row whose pair cannot be read is
// DROPPED rather than carried with the exchange symbol standing in — an
// instrument nothing can group is one a cross-venue view would show as a market
// of its own.
std::optional<SymbolText> canonical_of(std::string_view symbol) {
    const auto pair = split_symbol(symbol, '_');
    if (!pair.has_value()) {
        return std::nullopt;
    }
    return canonical_symbol(pair->first.view(), pair->second.view());
}

} // namespace

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.in_delisting || wire.status != "trading") {
        return std::nullopt;
    }

    const auto tick = parse_decimal_e9(wire.order_price_round);
    if (!tick.has_value() || tick->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Gate, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    const auto canonical = canonical_of(wire.name);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = FixedE9::from_raw(k_e9);
    const auto quanto = parse_decimal_e9(wire.quanto_multiplier);
    if (!quanto.has_value() || quanto->raw <= 0) {
        return std::nullopt;
    }
    base.contract_value = *quanto;
    // Unconditional because gate_venue only ever requests settle=usdt.
    // /futures/btc/contracts returns inverse contracts against this same
    // parser, so a new settle currency means deciding this per row.
    base.kind = ContractKind::Linear;
    PerpInstrument perp{base, wire.funding_interval_s};
    // UNDEF where it will not parse, never clamped to zero: a floor of zero
    // would read as a venue that never pays.
    if (const auto limit = parse_decimal_e9(wire.funding_rate_limit);
        limit.has_value() && limit->raw > 0) {
        perp.funding_cap = *limit;
        perp.funding_floor = FixedE9::from_raw(-limit->raw);
    }
    return perp;
}

std::optional<Instrument> map_currency_pair(const WireCurrencyPair& wire) {
    // buyable/sellable are one-sided listings; treating them as tradable puts
    // orders on the side the venue refuses.
    if (wire.trade_status != "tradable") {
        return std::nullopt;
    }
    const auto step = [](std::uint32_t digits) -> std::optional<FixedE9> {
        if (digits > 9) {
            return std::nullopt; // finer than FixedE9 represents
        }
        std::int64_t raw = k_e9;
        for (std::uint32_t i = 0; i < digits; ++i) {
            raw /= 10;
        }
        return FixedE9::from_raw(raw);
    };
    const auto tick = step(wire.precision);
    const auto lot = step(wire.amount_precision);
    if (!tick.has_value() || !lot.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.id);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Gate, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    const auto canonical = canonical_of(wire.id);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto notional = to_e9(wire.min_quote_amount); notional.has_value()) {
        base.min_notional = *notional;
    }
    base.contract_value = FixedE9::from_raw(k_e9); // spot quotes base units
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

namespace {

// Fees are neither a price nor a size, so they truncate at 1e-9 where those
// refuse: Gate pads a futures fee to more decimals than FixedE9 carries, and
// an exact read would drop the whole fill over a rounding in the eleventh
// place.
FixedE9 fee_e9(std::string_view text) noexcept {
    const auto value = parse_rate_e9(text);
    return value.has_value() ? *value : FixedE9::from_raw(0);
}

bool copy_order_id(std::string_view text, ExchangeOrderId& out) noexcept {
    if (text.empty()) {
        return true; // absent is survivable; wrong is not
    }
    const auto id = ExchangeOrderId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// Our own ULID coming back inside Gate's `text`. Anything without the `t-`
// prefix is one of Gate's reserved sources (`api`, `web`, `liq-...`), and a
// value that does not fit the field cannot be one of ours either — the event
// stays unattributed rather than matched to a lookalike.
bool copy_client_id(std::string_view text, ExchangeClientOrderId& out) noexcept {
    const auto body = client_id_from_text(text);
    if (body.empty()) {
        return true;
    }
    const auto id = ExchangeClientOrderId::from(body);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

} // namespace

std::optional<QtyLots> order_leaves(const WireOrder& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    return to_lots(wire.left, base_of(*instrument));
}

std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    OrderEventType type{};
    switch (wire.state) {
    case WireOrderState::Open:
        if (!wire.accepted) {
            return std::nullopt; // a fill or an amend; usertrades owns it
        }
        type = OrderEventType::Accepted;
        break;
    case WireOrderState::Cancelled:
        type = OrderEventType::Canceled;
        break;
    case WireOrderState::Expired:
        type = OrderEventType::Expired;
        break;
    case WireOrderState::Filled:
        // The user trade that took leaves to zero already said this, with the
        // exec id and the fee attached. Saying it twice attributes one fill to
        // two events.
        return std::nullopt;
    case WireOrderState::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = wire.sell ? Side::Sell : Side::Buy;
    // Futures has no order-type field at all: a price of zero with tif ioc IS
    // the market order, and that is the only thing that distinguishes it.
    out.order_type = (wire.order_type == "market" || (wire.price == "0" && wire.tif == "ioc"))
                         ? OrderType::Market
                         : OrderType::Limit;
    if (wire.tif == "ioc") {
        out.tif = TimeInForce::Ioc;
    } else if (wire.tif == "fok") {
        out.tif = TimeInForce::Fok;
    } else if (wire.tif == "poc") {
        out.flags = OrderFlags::PostOnly;
    }
    if (!copy_client_id(wire.text, out.exchange_client_order_id) ||
        !copy_order_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.amount, base); qty.has_value()) {
        out.qty = *qty;
    }
    out.leaves_qty = to_lots(wire.left, base).value_or(QtyLots::from_raw(0));
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_user_trade(const WireUserTrade& wire,
                                         const exec::InstrumentTable& table,
                                         QtyLots leaves,
                                         const ExchangeClientOrderId& known_client_id,
                                         UnixNanos ts_init) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = wire.sell ? Side::Sell : Side::Buy;
    out.liquidity = wire.maker ? Liquidity::Maker : Liquidity::Taker;
    out.exchange_client_order_id = known_client_id;
    if (!copy_client_id(wire.text, out.exchange_client_order_id) ||
        !copy_order_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (!wire.trade_id.empty()) {
        const auto exec_id = ExecId::from(wire.trade_id);
        if (!exec_id.has_value()) {
            return std::nullopt; // a dedupe key we cannot store is not one
        }
        out.exec_id = *exec_id;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = leaves.is_undef() ? QtyLots::from_raw(0) : leaves;
    out.fee = fee_e9(wire.fee);
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = to_lots(wire.amount, base);
    const auto leaves = to_lots(wire.left, base);
    if (!qty.has_value() || !leaves.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = wire.sell ? Side::Sell : Side::Buy;
    if (!copy_client_id(wire.text, out.exchange_client_order_id) ||
        !copy_order_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    switch (wire.state) {
    case WireOrderState::Open:
        out.status = leaves->raw < qty->raw ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
        break;
    case WireOrderState::Filled:
        out.status = OrderStatus::Filled;
        break;
    case WireOrderState::Cancelled:
        out.status = OrderStatus::Canceled;
        break;
    case WireOrderState::Expired:
        out.status = OrderStatus::Expired;
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

// A wallet figure, not a price: Gate sends twelve fractional digits and the
// exact reader refuses every one of them, which showed as an account holding
// nothing at all.
std::optional<FixedE9> to_amount(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

std::optional<BalanceReport> map_futures_account(const WireFuturesAccount& wire) {
    const auto asset = to_asset_code(wire.currency);
    if (!asset.has_value()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    // Each field left UNDEF when Gate omitted it or sent something this cannot
    // read exactly. Undef is what a reader checks; a zero here would be a claim
    // that the account holds nothing.
    if (const auto total = to_amount(wire.total); total.has_value()) {
        out.total = *total;
    }
    if (const auto available = to_amount(wire.available); available.has_value()) {
        out.available = *available;
    }
    if (const auto unrealized = to_amount(wire.unrealised_pnl); unrealized.has_value()) {
        out.unrealized = *unrealized;
    }
    out.ts_venue = UnixNanos{wire.update_s * 1'000'000'000ULL};
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    // Gate signs the position size the same way it signs an order size, so the
    // magnitude converts and the sign is re-applied afterwards.
    auto text = wire.size;
    const bool short_side = text.starts_with('-');
    if (short_side) {
        text.remove_prefix(1);
    }
    const auto size = to_lots(text, base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = short_side ? QtyLots::from_raw(-size->raw) : *size;
    if (const auto entry = to_ticks(wire.entry_price, base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    out.ts_venue = s_to_unix(wire.update_s);
    return out;
}

} // namespace raikiri::venues::gate
