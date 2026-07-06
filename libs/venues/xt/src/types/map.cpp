#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/map.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace raikiri::venues::xt {

namespace {

// SymbolText's capacity; the fold buffer must not outgrow what the table holds.
constexpr std::size_t k_max_symbol_text = 24;

// AssetCode's capacity; the fold buffer must not outgrow what the row holds.

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means XT changed an instrument's precision, so the message is
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

std::optional<FixedE9> positive_e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw <= 0) {
        return std::nullopt;
    }
    return *value;
}

std::optional<std::int64_t> pow10(std::int64_t n) noexcept {
    if (n < 0 || n > 18) {
        return std::nullopt;
    }
    std::int64_t out = 1;
    for (std::int64_t i = 0; i < n; ++i) {
        out *= 10;
    }
    return out;
}

const InstrumentBase* base_for(std::string_view symbol, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

// The private tree echoes `BTC_USDT` where the catalogue said `btc_usdt`, so a
// straight lookup misses every private record. Folding here rather than at load
// keeps the table keyed by the spelling that goes back out on an order.
const InstrumentBase*
base_for_private(std::string_view symbol, const exec::InstrumentTable& table, MarketType market) {
    if (symbol.empty() || symbol.size() > k_max_symbol_text) {
        return nullptr;
    }
    std::array<char, k_max_symbol_text> folded{};
    std::ranges::transform(symbol, folded.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    const auto* base = base_for(std::string_view{folded.data(), symbol.size()}, table);
    if (base == nullptr) {
        base = base_for(symbol, table);
    }
    // XT names a spot pair and a perpetual identically, so one table holds at
    // most one of them. Booking a spot fill against a perpetual's contract size
    // is a position error nothing downstream can detect.
    return (base != nullptr && base->id.market() == market) ? base : nullptr;
}

// Fees are neither a price nor a size, so they truncate at 1e-9 where those
// refuse: dropping a whole fill over a rounding in the eleventh decimal place
// would lose the fill, and the fee is the only field that can afford it.
FixedE9 fee_e9(std::string_view text) noexcept {
    const auto value = parse_rate_e9(text);
    return value.has_value() ? *value : FixedE9::from_raw(0);
}

void copy_client_id(std::string_view text, ExchangeClientOrderId& out) {
    // XT's own web orders carry ids like `myOrder123`, which is not one of our
    // encodings. Leaving the field UNSET is right: an order we did not place is
    // still one the reconciler has to see, and a foreign id must never be
    // decoded into a lookalike of one we minted.
    if (const auto id = decode_client_id(text); id.has_value()) {
        out = *id;
    }
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

bool copy_exec_id(std::string_view text, ExecId& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = ExecId::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// Ordered minus executed, floored at zero. A venue reporting more executed than
// ordered is a schema event; reading the difference as unsigned would turn it
// into an enormous resting size.
QtyLots leaves_from(std::string_view ordered, std::string_view done, const InstrumentBase& base) {
    const auto total = to_lots(ordered, base);
    if (!total.has_value()) {
        return QtyLots::from_raw(0);
    }
    const auto filled = to_lots(done, base);
    const auto left = total->raw - (filled.has_value() ? filled->raw : 0);
    return QtyLots::from_raw(left > 0 ? left : 0);
}

// New is the only working state XT reports as an event of its own; a fill is
// the trade record's to announce, and restating it here doubles the volume.
std::optional<OrderEventType> lifecycle_of(WireOrderStatus status) noexcept {
    switch (status) {
    case WireOrderStatus::New:
        return OrderEventType::Accepted;
    case WireOrderStatus::Cancelled:
        return OrderEventType::Canceled;
    case WireOrderStatus::Rejected:
        return OrderEventType::Rejected;
    case WireOrderStatus::Expired:
        return OrderEventType::Expired;
    case WireOrderStatus::PartiallyFilled:
    case WireOrderStatus::Filled:
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<OrderStatus> status_of(WireOrderStatus status, bool partially_filled) noexcept {
    switch (status) {
    case WireOrderStatus::New:
        return partially_filled ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
    case WireOrderStatus::PartiallyFilled:
        return OrderStatus::PartiallyFilled;
    case WireOrderStatus::Filled:
        return OrderStatus::Filled;
    case WireOrderStatus::Cancelled:
        return OrderStatus::Canceled;
    case WireOrderStatus::Rejected:
        return OrderStatus::Rejected;
    case WireOrderStatus::Expired:
        return OrderStatus::Expired;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

// The record's own word wins; the memo answers only when the record is silent.
std::optional<Side> side_of(std::string_view stated, const OrderContext& context) noexcept {
    if (stated == "SELL") {
        return Side::Sell;
    }
    if (stated == "BUY") {
        return Side::Buy;
    }
    return context.side_known ? std::optional{context.side} : std::nullopt;
}

Liquidity liquidity_of(bool known, bool maker) noexcept {
    // A fill whose side of the book XT did not state stays Unknown: guessing
    // Taker is how a maker strategy scores itself against a number it invented.
    if (!known) {
        return Liquidity::Unknown;
    }
    return maker ? Liquidity::Maker : Liquidity::Taker;
}

// Top of one side of a snapshot. XT sorts bids descending and asks ascending
// (verified across 528 frames), so element zero is the touch.
const WireLevel* top(std::span<const WireLevel> side) noexcept {
    return side.empty() ? nullptr : &side.front();
}

// XT names the pair nowhere but the symbol, lowercase `btc_usdt` on both
// catalogues; split_symbol folds the case. A row whose pair cannot be read is
// DROPPED rather than carried with the exchange symbol standing in — an
// instrument nothing can group is one a cross-venue view would show as a market
// of its own.
//
// A dated contract is spelled `btc_usdt_261225` and would be refused for its
// second separator, but productType turns it away before this is reached.
std::optional<SymbolText> canonical_of(std::string_view symbol) {
    const auto pair = split_symbol(symbol, '_');
    if (!pair.has_value()) {
        return std::nullopt;
    }
    return canonical_symbol(pair->first.view(), pair->second.view());
}

} // namespace

// Defined below beside the balance readers, and declared here because a
// turnover wants the same forgiving parse a wallet figure does: the exact
// readers refuse a number with too many fractional digits, which would show
// a market that traded as one that did not.
std::optional<FixedE9> to_amount(std::string_view text);

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_from_precision(std::int64_t digits) noexcept {
    if (digits < 0 || digits > 9) {
        return std::nullopt;
    }
    const auto scale = pow10(digits);
    if (!scale.has_value()) {
        return std::nullopt;
    }
    return FixedE9::from_raw(k_e9 / *scale);
}

std::optional<Bbo>
map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto* best_bid = top(wire.bids);
    const auto* best_ask = top(wire.asks);
    if (best_bid == nullptr || best_ask == nullptr) {
        return std::nullopt;
    }
    const auto bid = to_ticks(best_bid->price, *base);
    const auto ask = to_ticks(best_ask->price, *base);
    const auto bid_qty = to_lots(best_bid->qty, *base);
    const auto ask_qty = to_lots(best_ask->qty, *base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base->id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    BookSnapshot out{};
    out.instrument = base->id;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    out.venue_seq = wire.update_id;

    const auto fill = [&](std::span<const WireLevel> src,
                          std::array<BookLevel, k_book_levels>& dst) {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, *base);
            const auto qty = to_lots(level.qty, *base);
            // A level that does not divide by the instrument's step truncates
            // the book here rather than being rounded into a price XT never
            // quoted.
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

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.qty, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    TradeTick out{};
    out.instrument = base->id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

// One record out of everything both sources have said. Fields nobody has
// stated yet stay UNDEF: the reader checks for that, and a zero would be a
// number this process invented.
[[nodiscard]] FundingUpdate
merged(const InstrumentBase& base, const FundingMemo& memo, UnixNanos ts_event, UnixNanos ts_init) {
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = memo.rate;
    out.mark_price = memo.mark;
    out.next_funding_time = memo.next_funding;
    out.interval_s = memo.interval_s;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                         const exec::InstrumentTable& table,
                                         FundingMemo& memo,
                                         UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto rate = parse_decimal_e9(wire.rate_token);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    memo.rate = *rate;
    memo.next_funding = ms_to_unix(wire.next_collection_ms, UnixNanos{0});
    if (wire.interval_h > 0 && wire.interval_h <= 24) {
        memo.interval_s = static_cast<std::uint32_t>(wire.interval_h * 3600);
    }
    // XT states no publish time on this endpoint.
    return merged(*base, memo, ts_init, ts_init);
}

std::optional<FundingUpdate> map_mark_price(const WireMarkPrice& wire,
                                            const exec::InstrumentTable& table,
                                            FundingMemo& memo,
                                            UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto mark = to_ticks(wire.mark_price, *base);
    if (!mark.has_value()) {
        return std::nullopt; // a mark this contract's tick cannot express is no mark
    }
    memo.mark = *mark;
    return merged(*base, memo, ms_to_unix(wire.ts_ms, ts_init), ts_init);
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // ABSENT IS NOT ZERO. A frame that restated neither column says nothing
    // about the window, and a zero written here erases what the board had.
    if (wire.volume_24h_base.empty() && wire.volume_24h_quote.empty() &&
        wire.contracts_24h.empty()) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base->id;
    if (const auto coins = to_amount(wire.volume_24h_base); coins.has_value()) {
        out.volume_24h_base = *coins;
    } else if (const auto contracts = to_amount(wire.contracts_24h); contracts.has_value()) {
        out.volume_24h_base = turnover_e9(*contracts, base->contract_value);
    }
    if (const auto quote = parse_turnover_e6(wire.volume_24h_quote); quote.has_value()) {
        out.volume_24h_quote = *quote;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

namespace {

// XT files ETFs under INDEX; the shared aliases lift the true indices back out.
[[nodiscard]] AssetClass asset_class_of(std::string_view tags) noexcept {
    const auto has = [&](std::string_view tag) {
        return tags.find(std::string{"\""} + std::string{tag} + "\"") != std::string_view::npos;
    };
    if (tags.empty() || tags == "null" || tags == "[]") {
        return AssetClass::Crypto;
    }
    if (has("METAL") || has("COMMODITY")) {
        return AssetClass::Commodity;
    }
    if (has("FOREX")) {
        return AssetClass::FX;
    }
    if (has("STOCK") || has("INDEX")) {
        return AssetClass::Equity;
    }
    return AssetClass::Unknown;
}

} // namespace

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.product_type != "perpetual") {
        return std::nullopt;
    }
    if (wire.state != 0 || !wire.trade_switch || !wire.open_api) {
        return std::nullopt;
    }

    const auto tick = positive_e9(wire.tick_size);
    const auto lot = step_from_precision(wire.qty_precision);
    const auto contract_size = positive_e9(wire.contract_size);
    if (!tick.has_value() || !lot.has_value() || !contract_size.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Xt, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    const auto canonical = canonical_of(wire.symbol);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.asset_class = asset_class_of(wire.tags);
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min_notional = positive_e9(wire.min_notional); min_notional.has_value()) {
        base.min_notional = *min_notional;
    }
    base.contract_value = *contract_size;
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire) {
    // The two switches are the real gate: a pair listed and priced for the web
    // is not necessarily open to the API, and an order on one is refused.
    if (wire.state != "ONLINE" || !wire.trading_enabled || !wire.openapi_enabled) {
        return std::nullopt;
    }
    const auto tick = step_from_precision(wire.price_precision);
    const auto lot = step_from_precision(wire.qty_precision);
    if (!tick.has_value() || !lot.has_value()) {
        return std::nullopt; // finer than FixedE9 represents
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Xt, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    const auto canonical = canonical_of(wire.symbol);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.asset_class = AssetClass::Crypto;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto notional = positive_e9(wire.min_quote_qty); notional.has_value()) {
        base.min_notional = *notional;
    }
    base.contract_value = FixedE9::from_raw(k_e9); // spot quotes base units
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

std::optional<OrderEvent> map_futures_order_event(const WireFuturesOrder& wire,
                                                  const exec::InstrumentTable& table,
                                                  const ExchangeClientOrderId& fallback_client_id,
                                                  UnixNanos ts_init) {
    const auto* base = base_for_private(wire.symbol, table, MarketType::Perp);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto type = lifecycle_of(wire.state);
    if (!type.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = *type;
    out.instrument = base->id;
    out.side = wire.order_side == "SELL" ? Side::Sell : Side::Buy;
    out.order_type = wire.order_type == "MARKET" ? OrderType::Market : OrderType::Limit;
    if (wire.time_in_force == "GTX") {
        out.flags = OrderFlags::PostOnly;
    }
    // The socket push carries no clientOrderId at all, so the caller's memo is
    // the only thing that can attribute it.
    out.exchange_client_order_id = fallback_client_id;
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.orig_qty, *base); qty.has_value()) {
        out.qty = *qty;
    }
    // Nothing rests after a terminal event, PARTIALLY_CANCELED included: the
    // volume it took was reported by its own trade and the rest is gone.
    out.leaves_qty = *type == OrderEventType::Accepted ? out.qty : QtyLots::from_raw(0);
    out.ts_event = ms_to_unix(wire.updated_ms != 0 ? wire.updated_ms : wire.created_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_futures_deal(const WireFuturesDeal& wire,
                                           const exec::InstrumentTable& table,
                                           const OrderContext& context,
                                           UnixNanos ts_init) {
    const auto* base = base_for_private(wire.symbol, table, MarketType::Perp);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.quantity, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    // /order/trade-list states no side at all; the socket push does. When
    // neither the record nor the memo says, the fill is dropped — booking it
    // to a guessed direction moves the position by twice the fill.
    const auto side = side_of(wire.order_side, context);
    if (!side.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base->id;
    out.side = *side;
    out.exchange_client_order_id = context.client_order_id;
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id) ||
        !copy_exec_id(wire.exec_id, out.exec_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = context.leaves;
    out.fee = fee_e9(wire.fee);
    out.liquidity = liquidity_of(wire.liquidity_known, wire.maker);
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_futures_open_order(const WireFuturesOrder& wire,
                                                              const exec::InstrumentTable& table) {
    const auto* base = base_for_private(wire.symbol, table, MarketType::Perp);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto qty = to_lots(wire.orig_qty, *base);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    const auto leaves = leaves_from(wire.orig_qty, wire.executed_qty, *base);
    const auto status = status_of(wire.state, leaves.raw < qty->raw);
    if (!status.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base->id;
    out.side = wire.order_side == "SELL" ? Side::Sell : Side::Buy;
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves = leaves;
    out.status = *status;
    out.ts_venue =
        ms_to_unix(wire.updated_ms != 0 ? wire.updated_ms : wire.created_ms, UnixNanos{0});
    return out;
}

// A wallet figure, not a price: XT pads a balance well past nine fractional
// digits and the exact reader refuses the whole number, which shows as an
// account holding nothing at all.
std::optional<FixedE9> to_amount(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

std::optional<exec::PositionReport> map_futures_position(const WireFuturesPosition& wire,
                                                         const exec::InstrumentTable& table) {
    const auto* base = base_for_private(wire.symbol, table, MarketType::Perp);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto size = to_lots(wire.position_size, *base);
    if (!size.has_value() || size->raw == 0) {
        return std::nullopt; // absent means flat; a zero row states nothing
    }

    exec::PositionReport out{};
    out.instrument = base->id;
    // positionSize is UNSIGNED and positionSide is the only field that says
    // which way the position points; dropping the sign reads a short as a long.
    out.net_qty = QtyLots::from_raw(wire.position_side == "SHORT" ? -size->raw : size->raw);
    if (const auto entry = to_ticks(wire.entry_price, *base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    return out;
}

std::optional<LedgerEntry>
map_bill(const WireBill& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    LedgerKind kind{};
    if (wire.type == "FUND") {
        kind = LedgerKind::Funding;
    } else if (wire.type == "FEE") {
        kind = LedgerKind::Fee;
    } else {
        // CLOSE_POSITION is realised PnL, which the fill that closed the
        // position already booked; anything else is not trading money.
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    // VERBATIM. XT signs the amount itself and `side` restates that sign, so
    // applying `side` here would flip every SUB row — see account.hpp.
    const auto amount = to_amount(wire.amount);
    if (!amount.has_value() || amount->raw == 0) {
        return std::nullopt; // an unreadable or absent figure is not a zero charge
    }

    LedgerEntry charge{};
    charge.instrument = base->id;
    charge.amount = *amount;
    charge.kind = kind;
    charge.settled_at = ms_to_unix(wire.created_ms, UnixNanos{0});
    // XT mints an id per row, which is what tells two settlements on one
    // instrument apart when the stamps collide.
    if (const auto id = ExecId::from(wire.id); id.has_value()) {
        charge.reference = *id;
    }
    charge.ts_init = ts_init;
    return charge;
}

std::optional<BalanceReport> map_futures_balance(const WireFuturesBalance& wire) {
    const auto asset = to_asset_code(wire.coin);
    if (!asset.has_value()) {
        return std::nullopt;
    }

    BalanceReport out{};
    out.asset = *asset;
    // Each figure left UNDEF where XT omitted it or sent something this cannot
    // read. Undef is what a reader checks; a zero here would be a claim that the
    // account holds nothing.
    if (const auto total = to_amount(wire.wallet_balance); total.has_value()) {
        out.total = *total;
    }
    if (const auto available = to_amount(wire.available_balance); available.has_value()) {
        out.available = *available;
    }
    // `unrealized` and `ts_venue` stay unset: /balance/list carries neither a
    // PnL field nor a clock, and walletBalance minus a deposit history is our
    // arithmetic, not XT's.
    return out;
}

std::optional<OrderEvent> map_spot_order_event(const WireSpotOrder& wire,
                                               const exec::InstrumentTable& table,
                                               UnixNanos ts_init) {
    const auto* base = base_for_private(wire.symbol, table, MarketType::Spot);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto type = lifecycle_of(wire.state);
    if (!type.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = *type;
    out.instrument = base->id;
    out.side = wire.side == "SELL" ? Side::Sell : Side::Buy;
    out.order_type = wire.type == "MARKET" ? OrderType::Market : OrderType::Limit;
    if (wire.time_in_force == "GTX") {
        out.flags = OrderFlags::PostOnly;
    }
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    const auto qty = to_lots(wire.orig_qty, *base);
    if (!qty.has_value()) {
        return std::nullopt; // an UNDEF quantity must never reach the sink
    }
    out.qty = *qty;
    out.leaves_qty = *type == OrderEventType::Accepted ? out.qty : QtyLots::from_raw(0);
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_spot_open_order(const WireSpotOrder& wire,
                                                           const exec::InstrumentTable& table) {
    const auto* base = base_for_private(wire.symbol, table, MarketType::Spot);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto qty = to_lots(wire.orig_qty, *base);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    // XT states the remainder outright, so nothing is derived where it does.
    // The field is documented as 0 on a cancelled or rejected order, which is
    // why the subtraction is only the fallback.
    const auto stated = to_lots(wire.leaving_qty, *base);
    const auto leaves =
        stated.has_value() ? *stated : leaves_from(wire.orig_qty, wire.executed_qty, *base);
    const auto status = status_of(wire.state, leaves.raw < qty->raw);
    if (!status.has_value()) {
        return std::nullopt;
    }

    exec::OrderStatusReport out{};
    out.instrument = base->id;
    out.side = wire.side == "SELL" ? Side::Sell : Side::Buy;
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves = leaves;
    out.status = *status;
    out.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{0});
    return out;
}

std::optional<OrderEvent> map_spot_trade(const WireSpotTrade& wire,
                                         const exec::InstrumentTable& table,
                                         const OrderContext& context,
                                         UnixNanos ts_init) {
    const auto* base = base_for_private(wire.symbol, table, MarketType::Spot);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.quantity, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    // The socket push states no side — `b` is about the print's buyer, not
    // about us — so /v4/trade's `orderSide` or the memo is the only source.
    const auto side = side_of(wire.side, context);
    if (!side.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base->id;
    out.side = *side;
    out.exchange_client_order_id = context.client_order_id;
    if (!copy_id(wire.order_id, out.exchange_order_id) ||
        !copy_exec_id(wire.trade_id, out.exec_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = context.leaves;
    out.fee = fee_e9(wire.fee);
    out.liquidity = liquidity_of(wire.liquidity_known, wire.maker);
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::xt
