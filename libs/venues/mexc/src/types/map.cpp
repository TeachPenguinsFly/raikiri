#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/mexc/map.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>

namespace raikiri::venues::mexc {

namespace {

// /contract/detail `state`: 0 is enabled
constexpr std::int64_t k_state_enabled = 0;

constexpr std::int64_t k_max_scale = 9;

constexpr std::int64_t k_position_long = 1;
constexpr std::int64_t k_position_short = 2;

// exact conversion; a value off the step is dropped, not rounded
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(text); // may be scientific, e.g. 8.390E-7
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

// spot books carry sizes finer than the order step, so levels
// truncate to lots; orders still convert exactly
std::optional<QtyLots> to_lots_floor(std::string_view text, const InstrumentBase& base) {
    const auto qty = parse_decimal_e9(text);
    if (!qty.has_value() || base.lot_size.raw <= 0) {
        return std::nullopt;
    }
    return QtyLots::from_raw(qty->raw / base.lot_size.raw);
}

// rates keep their e9 form rather than ticks
std::optional<FixedE9> to_e9(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? std::optional{*value} : std::nullopt;
}

// turnover truncates below 1e-9 instead of failing
std::optional<FixedE9> to_amount(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

const InstrumentBase* base_for(std::string_view symbol, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<FixedE9> step_from_scale(std::int64_t scale) noexcept {
    if (scale < 0 || scale > k_max_scale) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::int64_t i = 0; i < scale; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

std::optional<Bbo> map_spot_bbo(const StreamQuote& wire,
                                std::string_view symbol,
                                std::uint64_t ts_ms,
                                const exec::InstrumentTable& table,
                                UnixNanos ts_init) {
    const auto* base = base_for(symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto bid = to_ticks(wire.bid_price, *base);
    const auto ask = to_ticks(wire.ask_price, *base);
    const auto bid_qty = to_lots_floor(wire.bid_qty, *base);
    const auto ask_qty = to_lots_floor(wire.ask_qty, *base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = base->id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_spot_trade(const StreamTrade& wire,
                                        std::string_view symbol,
                                        const exec::InstrumentTable& table,
                                        UnixNanos ts_init) {
    const auto* base = base_for(symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots_floor(wire.qty, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base->id;
    out.price = *price;
    out.qty = *qty;
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> map_spot_book(const StreamBook& wire,
                                          std::string_view symbol,
                                          std::uint64_t ts_ms,
                                          const exec::InstrumentTable& table,
                                          UnixNanos ts_init) {
    const auto* base = base_for(symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto fill = [&](std::span<const WireLevel> from,
                          std::array<BookLevel, k_book_levels>& into) -> std::uint32_t {
        std::uint32_t count = 0;
        for (const auto& level : from) {
            if (count == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, *base);
            const auto qty = to_lots_floor(level.qty, *base);
            if (!price.has_value() || !qty.has_value()) {
                return 0; // a level that will not convert makes the book a guess
            }
            into[count] = BookLevel{.price = *price, .qty = *qty};
            ++count;
        }
        return count;
    };
    BookSnapshot out{};
    out.instrument = base->id;
    out.bid_count = fill(wire.bids, out.bids);
    out.ask_count = fill(wire.asks, out.asks);
    if (out.bid_count == 0 && out.ask_count == 0) {
        return std::nullopt;
    }
    out.ts_event = ms_to_unix(ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<Bbo> map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!wire.snapshot || wire.bids.empty() || wire.asks.empty()) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto bid = to_ticks(wire.bids.front().price, *base);
    const auto ask = to_ticks(wire.asks.front().price, *base);
    const auto bid_qty = to_lots(wire.bids.front().qty, *base);
    const auto ask_qty = to_lots(wire.asks.front().qty, *base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base->id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> map_book(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (!wire.snapshot) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    BookSnapshot out{};
    out.instrument = base->id;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    if (wire.version > 0) {
        out.venue_seq = static_cast<std::uint64_t>(wire.version);
    }

    const auto fill = [&](std::span<const WireLevel> src, std::array<BookLevel, k_book_levels>& dst) {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = to_ticks(level.price, *base);
            const auto qty = to_lots(level.qty, *base);
            // an off-grid level truncates the book here
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

std::optional<TradeTick> map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto* base = &base_of(*instrument);
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base->id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.fair_price, *base); mark.has_value()) {
        out.mark_price = *mark;
    }
    // an index is off the tick grid, so it is floored onto it
    if (const auto index = parse_amount_e9(wire.index_price); index.has_value() && base->tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(index->raw / base->tick_size.raw);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    if (const auto* perp = std::get_if<PerpInstrument>(instrument); perp != nullptr) {
        out.interval_s = perp->funding_interval_s;
        // the ticker states no settle time; nextSettleTime is on the utc interval grid
        if (out.interval_s > 0) {
            const std::uint64_t interval_ns = std::uint64_t{out.interval_s} * 1'000'000'000ULL;
            out.next_funding_time = UnixNanos{(out.ts_event.ns / interval_ns + 1) * interval_ns};
        }
    }
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }

    FundingUpdate out{};
    out.instrument = base->id;
    out.rate = *rate;
    if (const auto mark = to_ticks(wire.fair_price, *base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.next_funding_time = ms_to_unix(wire.next_settle_ms, UnixNanos{0});
    if (wire.collect_cycle_h > 0) {
        out.interval_s = static_cast<std::uint32_t>(wire.collect_cycle_h * 3600);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

namespace {

// contracts into coins; an inverse contract is dollars, so the frame's last price converts it
FixedE9 open_interest_of(const WireTicker& wire, const InstrumentBase& base) {
    const auto contracts = to_amount(wire.open_interest);
    if (wire.open_interest.empty() || !contracts.has_value() || base.contract_value.is_undef()) {
        return FixedE9::undef();
    }
    const auto units = turnover_e9(*contracts, base.contract_value);
    if (base.kind != ContractKind::Inverse || units.is_undef()) {
        return units;
    }
    const auto last = to_amount(wire.last_price);
    if (!last.has_value() || last->raw <= 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(units.raw, k_e9, last->raw));
}

} // namespace

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // absent turnover leaves the previous figure in place
    if (wire.volume_24h_quote.empty()) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto quote = parse_turnover_e6(wire.volume_24h_quote);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base->id;
    out.volume_24h_base = FixedE9::from_raw(0);
    if (base->kind == ContractKind::Inverse) {
        // amount24 is in the base coin here; a contract is a fixed dollar amount
        out.volume_24h_base = to_amount(wire.volume_24h_quote).value_or(FixedE9::from_raw(0));
        const auto contracts = to_amount(wire.volume_24h_contracts);
        if (!contracts.has_value() || base->contract_value.raw <= 0) {
            return std::nullopt;
        }
        out.volume_24h_quote = turnover_e6(*contracts, base->contract_value);
        out.open_interest = open_interest_of(wire, *base);
        out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
        out.ts_init = ts_init;
        return out;
    }
    if (const auto contracts = to_amount(wire.volume_24h_contracts);
        contracts.has_value() && base->contract_value.raw > 0 && !base->contract_value.is_undef()) {
        const auto units = mul_wide(contracts->raw, base->contract_value.raw).raw / k_e9;
        if (units <= std::numeric_limits<std::int64_t>::max()) {
            out.volume_24h_base = FixedE9::from_raw(static_cast<std::int64_t>(units));
        }
    }
    out.volume_24h_quote = *quote;
    out.open_interest = open_interest_of(wire, *base);
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_spot_ticker_stats(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto volume = to_amount(wire.volume_base);
    const auto quote = parse_turnover_e6(wire.volume_quote);
    if (!volume.has_value() && !quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base->id;
    out.volume_24h_base = volume.value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = quote.value_or(FixedE6::from_raw(0));
    out.ts_event = ms_to_unix(wire.close_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

namespace {

[[nodiscard]] AssetClass asset_class_of(std::string_view plates) noexcept {
    const auto has = [&](std::string_view zone) {
        return plates.find(std::string{"\"mc-trade-zone-"} + std::string{zone} + "\"") != std::string_view::npos;
    };
    if (has("ETF")) {
        return AssetClass::Equity;
    }
    if (has("stockindex")) {
        return AssetClass::Index;
    }
    if (has("Stock") || has("japanstock") || has("preipo")) {
        return AssetClass::Equity;
    }
    if (has("Commodities") || has("metals") || has("metalsfutures") || has("OIL")) {
        return AssetClass::Commodity;
    }
    if (has("Forex")) {
        return AssetClass::FX;
    }
    if (has("tradfi")) {
        return AssetClass::Unknown;
    }
    return AssetClass::Crypto;
}

// "SILVER(XAG)" names the ticker in brackets; a plain name is the ticker itself
[[nodiscard]] std::string_view named_ticker(std::string_view name) noexcept {
    const auto open = name.find('(');
    if (open != std::string_view::npos && name.ends_with(')')) {
        name = name.substr(open + 1, name.size() - open - 2);
    }
    const bool plain = !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
    return plain ? name : std::string_view{};
}

} // namespace

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.state != k_state_enabled) {
        return std::nullopt;
    }

    const auto tick = step_from_scale(wire.price_scale);
    const auto lot = step_from_scale(wire.vol_scale);
    if (!tick.has_value() || !lot.has_value()) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }
    const auto contract_size = parse_number_e9(wire.contract_size); // NAS100: "1e-05"
    if (!contract_size.has_value() || contract_size->raw <= 0) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional; InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Mexc, MarketType::Perp, SymbolId{0});
    base.exchange_symbol = *symbol;
    // a row with no stated pair is dropped
    const auto canonical = canonical_symbol(wire.base_coin, wire.quote_coin);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *contract_size;
    base.kind = !wire.settle_coin.empty() && wire.settle_coin != wire.quote_coin ? ContractKind::Inverse
                                                                                 : ContractKind::Linear;
    base.asset_class = asset_class_of(wire.concept_plate);
    // the display name is the ticker other venues use: GOLDCIR, XAG, FIL for FILECOIN
    if (const auto name = UnderlyingText::from(named_ticker(wire.base_coin_name)); name.has_value()) {
        base.underlying = *name;
    }
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire) {
    // `status` is "1" on a trading row; isSpotTradingAllowed gates tradability
    if (wire.status != "1" || !wire.spot_trading_allowed) {
        return std::nullopt;
    }
    const auto tick = step_from_scale(wire.quote_precision);
    if (!tick.has_value()) {
        return std::nullopt;
    }
    // the quantity step is a decimal, not a digit count
    const auto lot = parse_decimal_e9(wire.base_size_step);
    if (!lot.has_value() || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional; InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Mexc, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    // a row missing either half is dropped
    const auto canonical = canonical_symbol(wire.base_asset, wire.quote_asset);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.asset_class = AssetClass::Crypto;
    base.tick_size = *tick;
    base.lot_size = *lot;
    // a spot lot is one base unit
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

namespace {

// 1 holding, 2 system auto-holding, 3 closed
constexpr std::int64_t k_position_closed = 3;

// futures order states; 2 covers new and partially filled
constexpr std::int64_t k_order_pending = 1;
constexpr std::int64_t k_order_working = 2;
constexpr std::int64_t k_order_filled = 3;
constexpr std::int64_t k_order_cancelled = 4;
constexpr std::int64_t k_order_invalid = 5;

// a non-ulid externalOid (web orders use `_m_<hex>`) leaves the field unset and keeps the event
void copy_client_id(std::string_view text, ExchangeClientOrderId& out) {
    if (const auto id = ExchangeClientOrderId::from(text); id.has_value()) {
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

// ordered minus dealt, floored at zero
QtyLots leaves_from(std::string_view ordered, std::string_view dealt, const InstrumentBase& base) {
    const auto total = to_lots(ordered, base);
    if (!total.has_value()) {
        return QtyLots::from_raw(0);
    }
    const auto done = to_lots(dealt, base);
    const auto left = total->raw - (done.has_value() ? done->raw : 0);
    return QtyLots::from_raw(left > 0 ? left : 0);
}

} // namespace

Side side_from_futures(std::int64_t side) noexcept {
    // 1 open long and 2 close short buy; 3 open short and 4 close long sell
    return (side == 1 || side == 2) ? Side::Buy : Side::Sell;
}

std::optional<OrderEvent>
map_futures_order_event(const WireFuturesOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    OrderEventType type{};
    if (wire.state == k_order_pending) {
        return std::nullopt; // "uninformed": MEXC has not decided yet
    } else if (wire.state == k_order_working) {
        // state 2 covers new and partially filled; dealVol tells them apart
        const auto dealt = to_lots(wire.deal_vol, *base);
        if (dealt.has_value() && dealt->raw > 0) {
            return std::nullopt;
        }
        type = OrderEventType::Accepted;
    } else if (wire.state == k_order_filled) {
        return std::nullopt; // the deal frame carries the money
    } else if (wire.state == k_order_cancelled) {
        type = OrderEventType::Canceled;
    } else if (wire.state == k_order_invalid) {
        type = OrderEventType::Rejected;
    } else {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base->id;
    out.side = side_from_futures(wire.side);
    out.order_type = wire.order_type == 5 ? OrderType::Market : OrderType::Limit;
    copy_client_id(wire.external_oid, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    if (const auto qty = to_lots(wire.vol, *base); qty.has_value()) {
        out.qty = *qty;
    }
    out.leaves_qty = type == OrderEventType::Accepted ? out.qty : leaves_from(wire.vol, wire.deal_vol, *base);
    out.ts_event = ms_to_unix(wire.update_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_futures_deal(const WireFuturesDeal& wire,
                                           const exec::InstrumentTable& table,
                                           QtyLots leaves,
                                           const ExchangeClientOrderId& client_id,
                                           UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.vol, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base->id;
    out.side = side_from_futures(wire.side);
    out.exchange_client_order_id = client_id;
    if (!copy_id(wire.order_id, out.exchange_order_id) || !copy_exec_id(wire.deal_id, out.exec_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = leaves;
    if (const auto fee = to_e9(wire.fee); fee.has_value()) {
        out.fee = *fee;
    }
    // unknown liquidity stays Unknown
    out.liquidity = wire.liquidity_known ? (wire.taker ? Liquidity::Taker : Liquidity::Maker) : Liquidity::Unknown;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_futures_open_order(const WireFuturesOrder& wire,
                                                              const exec::InstrumentTable& table) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto qty = to_lots(wire.vol, *base);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    const auto leaves = leaves_from(wire.vol, wire.deal_vol, *base);

    exec::OrderStatusReport out{};
    out.instrument = base->id;
    out.side = side_from_futures(wire.side);
    copy_client_id(wire.external_oid, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (const auto price = to_ticks(wire.price, *base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves = leaves;
    out.status = wire.state == k_order_cancelled ? OrderStatus::Canceled
                 : wire.state == k_order_filled  ? OrderStatus::Filled
                 : wire.state == k_order_invalid ? OrderStatus::Rejected
                 : leaves.raw < qty->raw         ? OrderStatus::PartiallyFilled
                                                 : OrderStatus::Accepted;
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<exec::PositionReport> map_futures_position(const WireFuturesPosition& wire,
                                                         const exec::InstrumentTable& table) {
    if (wire.state == k_position_closed) {
        return std::nullopt;
    }
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto size = to_lots(wire.hold_vol, *base);
    if (!size.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base->id;
    // holdVol is unsigned; positionType gives the direction
    out.net_qty = QtyLots::from_raw(wire.position_type == k_position_short ? -size->raw : size->raw);
    if (const auto entry = to_ticks(wire.hold_avg_price, *base); entry.has_value()) {
        out.avg_entry = *entry;
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<OrderEvent>
map_spot_order_event(const WireSpotOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }

    OrderEventType type{};
    switch (wire.status) {
    case WireOrderStatus::New:
        type = OrderEventType::Accepted;
        break;
    case WireOrderStatus::Cancelled:
        type = OrderEventType::Canceled;
        break;
    case WireOrderStatus::Rejected:
        type = OrderEventType::Rejected;
        break;
    case WireOrderStatus::Expired:
        type = OrderEventType::Expired;
        break;
    case WireOrderStatus::PartiallyFilled:
    case WireOrderStatus::Filled:
        // the trade record reports the fill
        return std::nullopt;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base->id;
    out.side = wire.side == "SELL" ? Side::Sell : Side::Buy;
    out.order_type = wire.type == "MARKET" ? OrderType::Market : OrderType::Limit;
    if (wire.type == "LIMIT_MAKER") {
        out.flags = OrderFlags::PostOnly;
    }
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
    out.leaves_qty = type == OrderEventType::Accepted ? out.qty : leaves_from(wire.orig_qty, wire.executed_qty, *base);
    out.ts_event = ms_to_unix(wire.update_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_spot_open_order(const WireSpotOrder& wire,
                                                           const exec::InstrumentTable& table) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto qty = to_lots(wire.orig_qty, *base);
    if (!qty.has_value()) {
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
    out.leaves = leaves_from(wire.orig_qty, wire.executed_qty, *base);
    switch (wire.status) {
    case WireOrderStatus::New:
        out.status = OrderStatus::Accepted;
        break;
    case WireOrderStatus::PartiallyFilled:
        out.status = OrderStatus::PartiallyFilled;
        break;
    case WireOrderStatus::Filled:
        out.status = OrderStatus::Filled;
        break;
    case WireOrderStatus::Cancelled:
        out.status = OrderStatus::Canceled;
        break;
    case WireOrderStatus::Rejected:
        out.status = OrderStatus::Rejected;
        break;
    case WireOrderStatus::Expired:
        out.status = OrderStatus::Expired;
        break;
    case WireOrderStatus::Unknown:
        return std::nullopt;
    }
    out.ts_venue = ms_to_unix(wire.update_ms, UnixNanos{0});
    return out;
}

std::optional<OrderEvent>
map_spot_trade(const WireSpotTrade& wire, const exec::InstrumentTable& table, QtyLots leaves, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = to_ticks(wire.price, *base);
    const auto qty = to_lots(wire.qty, *base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base->id;
    out.side = wire.is_buyer ? Side::Buy : Side::Sell;
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_id(wire.order_id, out.exchange_order_id) || !copy_exec_id(wire.trade_id, out.exec_id)) {
        return std::nullopt;
    }
    out.price = *price;
    out.qty = *qty;
    out.leaves_qty = leaves;
    if (const auto fee = to_e9(wire.commission); fee.has_value()) {
        out.fee = *fee;
    }
    out.liquidity = wire.is_maker ? Liquidity::Maker : Liquidity::Taker;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<LedgerEntry>
map_funding_record(const WireFundingRecord& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = base_for(wire.symbol, table);
    if (base == nullptr) {
        return std::nullopt;
    }
    if (wire.position_type != k_position_long && wire.position_type != k_position_short) {
        return std::nullopt; // a direction we cannot read is not one to guess at
    }
    const auto amount = to_amount(wire.funding);
    if (!amount.has_value()) {
        return std::nullopt; // an unreadable figure is not a zero charge
    }
    const auto magnitude = amount->raw < 0 ? -amount->raw : amount->raw;
    if (magnitude == 0) {
        return std::nullopt; // a settlement that moved nothing is nothing to book
    }
    // a positive rate means longs pay; the sign is read from the token
    // since tiny rates truncate to zero
    const bool rate_negative = wire.rate.starts_with('-');
    const bool paid_away = (wire.position_type == k_position_long) != rate_negative;

    LedgerEntry charge{};
    charge.instrument = base->id;
    charge.amount = FixedE9::from_raw(paid_away ? -magnitude : magnitude);
    charge.kind = LedgerKind::Funding;
    charge.settled_at = ms_to_unix(wire.settle_ms, UnixNanos{});
    // mexc's row id separates settlements with the same stamp
    std::array<char, 24> buf{};
    if (const auto [stop, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), wire.id); ec == std::errc{}) {
        if (const auto id = ExecId::from(std::string_view{buf.data(), static_cast<std::size_t>(stop - buf.data())});
            id.has_value()) {
            charge.reference = *id;
        }
    }
    charge.ts_init = ts_init;
    return charge;
}

} // namespace raikiri::venues::mexc
