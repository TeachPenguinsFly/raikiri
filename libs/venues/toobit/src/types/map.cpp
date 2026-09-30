#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/toobit/map.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>

namespace raikiri::venues::toobit {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::int64_t k_one = 1'000'000'000LL;

std::optional<std::int64_t> exact(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

// digits past the ninth decimal are below every lot; they are cut before parsing.
std::string_view nine_places(std::string_view text) noexcept {
    const auto dot = text.find('.');
    return dot == std::string_view::npos || text.size() - dot <= 10 ? text : text.substr(0, dot + 10);
}

// spot books carry the odd size off the lot grid; it is floored, not refused.
std::optional<std::int64_t> floored(std::string_view text, FixedE9 step) {
    if (step.raw <= 0) {
        return std::nullopt;
    }
    if (const auto value = parse_decimal_e9(nine_places(text)); value.has_value()) {
        return value->raw < 0 ? std::nullopt : std::optional{value->raw / step.raw};
    }
    // meme-coin sizes pass nine billion, past what e9 holds; a whole lot divides the integer part.
    if (step.raw % k_one != 0) {
        return std::nullopt;
    }
    const auto whole = text.substr(0, text.find('.'));
    std::int64_t units = 0;
    const auto* const end = whole.data() + whole.size();
    if (const auto [stop, ec] = std::from_chars(whole.data(), end, units); ec != std::errc{} || stop != end) {
        return std::nullopt;
    }
    return units / (step.raw / k_one);
}

std::optional<FixedE9> ratio(FixedE9 num, FixedE9 den) noexcept {
    if (num.raw <= 0 || den.raw <= 0) {
        return std::nullopt;
    }
    constexpr auto k_max = std::numeric_limits<std::int64_t>::max();
    if (num.raw % den.raw == 0) {
        const auto whole = num.raw / den.raw;
        return whole <= k_max / k_one ? std::optional{FixedE9::from_raw(whole * k_one)} : std::nullopt;
    }
    if (k_one % den.raw == 0) {
        const auto scale = k_one / den.raw;
        return num.raw <= k_max / scale ? std::optional{FixedE9::from_raw(num.raw * scale)} : std::nullopt;
    }
    return std::nullopt;
}

template <std::size_t N> bool copy_text(std::string_view text, FixedString<N>& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = FixedString<N>::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

// an order placed elsewhere carries an id of another shape; it stays blank.
void copy_client_id(std::string_view text, ExchangeClientOrderId& out) {
    if (const auto id = ExchangeClientOrderId::from(text); id.has_value()) {
        out = *id;
    }
}

Side side_of(std::string_view text) noexcept {
    return text.starts_with("SELL") ? Side::Sell : Side::Buy;
}

OrderStatus status_of(std::string_view text) noexcept {
    if (text == "PARTIALLY_FILLED") {
        return OrderStatus::PartiallyFilled;
    }
    if (text == "FILLED") {
        return OrderStatus::Filled;
    }
    if (text == "CANCELED") {
        return OrderStatus::Canceled;
    }
    if (text == "PENDING_CANCEL") {
        return OrderStatus::PendingCancel;
    }
    if (text == "REJECTED") {
        return OrderStatus::Rejected;
    }
    return OrderStatus::Accepted;
}

std::optional<OrderEventType> event_of(std::string_view status) noexcept {
    if (status == "NEW") {
        return OrderEventType::Accepted;
    }
    if (status == "CANCELED") {
        return OrderEventType::Canceled;
    }
    if (status == "REJECTED") {
        return OrderEventType::Rejected;
    }
    return std::nullopt;
}

// a few contracts quote a level off their own tick; that level is skipped, not the book.
bool upsert(book::Ladder& ladder, const WireLevel& level, const InstrumentBase& base) {
    const auto qty = floored(level.qty, base.lot_size);
    if (!qty.has_value()) {
        return false;
    }
    const auto price = exact(level.price, base.tick_size);
    if (!price.has_value()) {
        return true;
    }
    if (*qty == 0) {
        ladder.remove(PriceTicks::from_raw(*price));
        return true;
    }
    ladder.upsert({.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)});
    return true;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * k_ns_per_ms};
}

std::uint32_t interval_of(std::string_view period) noexcept {
    if (period.size() < 2) {
        return 0;
    }
    std::uint32_t count = 0;
    const auto* const end = period.data() + period.size() - 1;
    if (const auto [stop, ec] = std::from_chars(period.data(), end, count); ec != std::errc{} || stop != end) {
        return 0;
    }
    switch (period.back()) {
    case 'H':
    case 'h':
        return count * 3600U;
    case 'M':
    case 'm':
        return count * 60U;
    default:
        return 0;
    }
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market, const WireFunding* funding) {
    if (wire.status != "TRADING") {
        return std::nullopt;
    }
    const bool perp = market == MarketType::Perp;
    if (perp != wire.contract || (perp && (wire.inverse || wire.margin != k_perp_margin))) {
        return std::nullopt;
    }
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return std::nullopt;
    }
    const auto tick = parse_decimal_e9(wire.tick);
    const auto step = parse_decimal_e9(wire.step);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(perp ? wire.underlying : wire.base, wire.quote);
    if (!tick.has_value() || !step.has_value() || tick->raw <= 0 || step->raw <= 0 || !symbol.has_value() ||
        !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Toobit, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.kind = ContractKind::Linear;
    base.tick_size = *tick;
    if (const auto min = parse_decimal_e9(wire.min_notional); min.has_value()) {
        base.min_notional = *min;
    }
    if (!perp) {
        base.lot_size = *step;
        base.contract_value = FixedE9::from_raw(k_one);
        return SpotInstrument{base};
    }
    const auto multiplier = parse_decimal_e9(wire.multiplier);
    if (!multiplier.has_value()) {
        return std::nullopt;
    }
    const auto lot = ratio(*step, *multiplier);
    if (!lot.has_value()) {
        return std::nullopt;
    }
    base.lot_size = *lot;
    base.contract_value = *multiplier;
    PerpInstrument out{};
    out.base = base;
    if (funding != nullptr) {
        out.funding_interval_s = interval_of(funding->period);
        if (const auto cap = parse_rate_e9(funding->cap); cap.has_value()) {
            out.funding_cap = *cap;
        }
        if (const auto floor = parse_rate_e9(funding->floor); floor.has_value()) {
            out.funding_floor = *floor;
        }
    }
    return out;
}

std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto bid = exact(wire.bid, base.tick_size);
    const auto ask = exact(wire.ask, base.tick_size);
    const auto bid_qty = floored(wire.bid_qty, base.lot_size);
    const auto ask_qty = floored(wire.ask_qty, base.lot_size);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value() || *bid_qty == 0 ||
        *ask_qty == 0) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = base.id;
    out.bid = PriceTicks::from_raw(*bid);
    out.ask = PriceTicks::from_raw(*ask);
    out.bid_qty = QtyLots::from_raw(*bid_qty);
    out.ask_qty = QtyLots::from_raw(*ask_qty);
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = exact(wire.price, base.tick_size);
    const auto qty = floored(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty == 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.buyer_maker ? Side::Sell : Side::Buy;
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<PriceTicks> map_mark(const WireMark& wire, const InstrumentBase& base) noexcept {
    const auto mark = parse_amount_e9(wire.price);
    if (!mark.has_value() || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(mark->raw / base.tick_size.raw);
}

std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, PriceTicks mark, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.rate);
    const auto interval = interval_of(wire.period);
    if (!rate.has_value() || interval == 0) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.predicted_rate = FixedE9::undef();
    out.mark_price = mark;
    out.next_funding_time = ms_to_unix(wire.next_ms, UnixNanos{});
    out.interval_s = interval;
    out.accrual = FundingAccrual::Discrete;
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.quote_volume);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = *quote;
    if (base.id.market() == MarketType::Spot) {
        if (const auto volume = parse_amount_e9(wire.volume); volume.has_value()) {
            out.volume_24h_base = *volume;
        }
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

DepthBook::Step DepthBook::apply(const WireDepth& wire, const InstrumentBase& base) {
    if (wire.first) {
        ladders_.clear();
        seeded_ = true;
        frames_ = 0;
    } else if (!seeded_) {
        return Step::Waiting;
    } else if (wire.ts_ms < ts_ms_) {
        reset();
        return Step::Broken;
    }
    ts_ms_ = wire.ts_ms;
    for (const auto& level : wire.bids) {
        if (!upsert(ladders_.bids(), level, base)) {
            reset();
            return Step::Broken;
        }
    }
    for (const auto& level : wire.asks) {
        if (!upsert(ladders_.asks(), level, base)) {
            reset();
            return Step::Broken;
        }
    }
    ++frames_;
    if (const auto touch = ladders_.touch(); touch.has_value() && touch->first.price.raw >= touch->second.price.raw) {
        reset();
        return Step::Broken;
    }
    return Step::Applied;
}

std::optional<BookSnapshot> DepthBook::snapshot(InstrumentId instrument, UnixNanos ts_init) const {
    if (!seeded_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument, frames_, ms_to_unix(ts_ms_, ts_init), ts_init);
}

void DepthBook::reset() noexcept {
    ladders_.clear();
    seeded_ = false;
    ts_ms_ = 0;
    frames_ = 0;
}

std::string_view order_side_text(Side side, OrderFlags flags) noexcept {
    const bool close = has_flag(flags, OrderFlags::ReduceOnly);
    if (side == Side::Buy) {
        return close ? "BUY_CLOSE" : "BUY_OPEN";
    }
    return close ? "SELL_CLOSE" : "SELL_OPEN";
}

std::optional<OrderEvent>
map_execution(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto last = floored(wire.last_qty, base.lot_size);
    const bool fill = last.has_value() && *last > 0;
    const auto type = fill ? std::optional{OrderEventType::Filled} : event_of(wire.status);
    if (!type.has_value()) {
        return std::nullopt;
    }
    OrderEvent out{};
    out.type = *type;
    out.instrument = base.id;
    out.side = side_of(wire.side);
    out.order_type = wire.order_type == "MARKET" ? OrderType::Market : OrderType::Limit;
    out.tif = wire.tif == "IOC" ? TimeInForce::Ioc : wire.tif == "FOK" ? TimeInForce::Fok : TimeInForce::Gtc;
    if (wire.tif == "POST_ONLY") {
        out.flags = OrderFlags::PostOnly;
    }
    if (wire.close) {
        out.flags = out.flags | OrderFlags::ReduceOnly;
    }
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_text(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    const auto total = floored(wire.qty, base.lot_size);
    const auto cum = floored(wire.cum_qty, base.lot_size);
    const auto leaves = total.has_value() && cum.has_value() ? *total - *cum : 0;
    out.leaves_qty = QtyLots::from_raw(leaves < 0 ? 0 : leaves);
    if (fill) {
        const auto price = exact(wire.last_price, base.tick_size);
        if (!price.has_value()) {
            return std::nullopt;
        }
        out.price = PriceTicks::from_raw(*price);
        out.qty = QtyLots::from_raw(*last);
        if (const auto fee = parse_amount_e9(wire.fee); fee.has_value()) {
            out.fee = *fee;
        }
        out.liquidity = wire.maker ? Liquidity::Maker : Liquidity::Taker;
        std::string exec_text{wire.order_id};
        exec_text.push_back('-');
        exec_text.append(wire.cum_qty);
        (void)copy_text(exec_text, out.exec_id);
    } else {
        out.price = PriceTicks::from_raw(exact(wire.price, base.tick_size).value_or(0));
        out.qty = QtyLots::from_raw(total.value_or(0));
    }
    out.ts_event = ms_to_unix(wire.update_ms != 0 ? wire.update_ms : wire.event_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_order(const WireOrder& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = side_of(wire.side);
    out.status = status_of(wire.status);
    copy_client_id(wire.client_order_id, out.exchange_client_order_id);
    if (!copy_text(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    const auto qty = floored(wire.orig_qty, base.lot_size);
    const auto done = floored(wire.executed_qty, base.lot_size);
    out.price = PriceTicks::from_raw(exact(wire.price, base.tick_size).value_or(0));
    out.qty = QtyLots::from_raw(qty.value_or(0));
    out.leaves = QtyLots::from_raw(qty.has_value() && done.has_value() ? *qty - *done : 0);
    out.ts_venue = ms_to_unix(wire.update_ms != 0 ? wire.update_ms : wire.time_ms, UnixNanos{});
    return out;
}

std::optional<exec::FillReport> map_user_trade(const WireUserTrade& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = exact(wire.price, base.tick_size);
    const auto qty = floored(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    exec::FillReport out{};
    if (!copy_text(wire.id, out.exec_id) || !copy_text(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.instrument = base.id;
    out.side = side_of(wire.side);
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    if (const auto fee = parse_amount_e9(wire.commission); fee.has_value()) {
        out.fee = *fee;
    }
    out.liquidity = wire.maker ? Liquidity::Maker : Liquidity::Taker;
    out.ts_venue = ms_to_unix(wire.time_ms, UnixNanos{});
    return out;
}

std::vector<exec::PositionReport>
map_positions(std::span<const WirePosition> rows, const exec::InstrumentTable& table, UnixNanos now) {
    struct Net {
        InstrumentId id;
        std::int64_t lots{0};
        std::int64_t largest{0};
        PriceTicks avg{};
    };

    std::vector<Net> nets;
    for (const auto& row : rows) {
        const auto* instrument = table.find(row.symbol);
        if (instrument == nullptr) {
            continue;
        }
        const auto& base = base_of(*instrument);
        const auto lots = floored(row.position, base.lot_size);
        if (!lots.has_value()) {
            continue;
        }
        auto it = std::find_if(nets.begin(), nets.end(), [&](const Net& net) { return net.id == base.id; });
        if (it == nets.end()) {
            nets.push_back(Net{.id = base.id, .lots = 0, .largest = 0, .avg = PriceTicks{}});
            it = std::prev(nets.end());
        }
        it->lots += row.side == "SHORT" ? -*lots : *lots;
        if (*lots > it->largest) {
            it->largest = *lots;
            const auto avg = parse_amount_e9(row.avg_price);
            if (avg.has_value() && base.tick_size.raw > 0) {
                it->avg = PriceTicks::from_raw(avg->raw / base.tick_size.raw);
            }
        }
    }
    std::vector<exec::PositionReport> out;
    out.reserve(nets.size());
    for (const auto& net : nets) {
        exec::PositionReport report{};
        report.instrument = net.id;
        report.net_qty = QtyLots::from_raw(net.lots);
        report.avg_entry = net.avg;
        report.ts_venue = now;
        out.push_back(report);
    }
    return out;
}

std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos now) {
    const auto asset = to_asset_code(wire.coin);
    if (!asset.has_value()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    if (const auto total = parse_amount_e9(wire.balance); total.has_value()) {
        out.total = *total;
    }
    if (const auto available = parse_amount_e9(wire.available); available.has_value()) {
        out.available = *available;
    }
    if (const auto unrealized = parse_amount_e9(wire.unrealized); unrealized.has_value()) {
        out.unrealized = *unrealized;
    }
    out.ts_venue = now;
    return out;
}

} // namespace raikiri::venues::toobit
