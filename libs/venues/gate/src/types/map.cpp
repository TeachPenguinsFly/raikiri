#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/options/convention.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/rest.hpp>

#include <array>
#include <optional>
#include <string>

namespace raikiri::venues::gate {

namespace {

// a value off the instrument's step is dropped rather than rounded
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

// rates keep their 1e9 scale
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

std::optional<Bbo> map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

namespace {

// levels stop at the domain's ceiling; the count says how many are real
std::uint32_t
fill_levels(std::span<const WireLevel> from, const InstrumentBase& base, std::array<BookLevel, k_book_levels>& into) {
    std::uint32_t count = 0;
    for (const auto& level : from) {
        if (count == k_book_levels) {
            break;
        }
        const auto price = to_ticks(level.price, base);
        const auto qty = to_lots(level.qty, base);
        if (!price.has_value() || !qty.has_value()) {
            return 0; // one bad level invalidates the book
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
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
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
        return std::nullopt; // contract quotes no funding
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
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireContract& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    WireTicker ticker{};
    ticker.contract = wire.name;
    ticker.funding_rate = wire.funding_rate;
    ticker.mark_price = wire.mark_price;
    ticker.funding_next_apply_s = wire.funding_next_apply_s;
    ticker.funding_interval_s = wire.funding_interval_s;
    return map_funding(ticker, table, ts_init);
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

// the pair is read from the symbol; an unreadable one drops the row
std::optional<SymbolText> canonical_of(std::string_view symbol) {
    const auto pair = split_symbol(symbol, '_');
    if (!pair.has_value()) {
        return std::nullopt;
    }
    return canonical_symbol(pair->first.view(), pair->second.view());
}

[[nodiscard]] AssetClass asset_class_of(std::string_view contract_type) noexcept {
    if (contract_type.empty()) {
        return AssetClass::Crypto;
    }
    if (contract_type == "stocks") {
        return AssetClass::Equity;
    }
    if (contract_type == "indices") {
        return AssetClass::Index;
    }
    if (contract_type == "commodities" || contract_type == "metals") {
        return AssetClass::Commodity;
    }
    if (contract_type == "forex") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
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
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Gate, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    const auto canonical = canonical_of(wire.name);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.asset_class = asset_class_of(wire.contract_type);
    base.tick_size = *tick;
    base.lot_size = FixedE9::from_raw(k_e9);
    const auto quanto = parse_decimal_e9(wire.quanto_multiplier);
    if (!quanto.has_value() || quanto->raw <= 0) {
        return std::nullopt;
    }
    base.contract_value = *quanto;
    // only settle=usdt is requested; other settle currencies return inverse contracts
    base.kind = ContractKind::Linear;
    PerpInstrument perp{base, wire.funding_interval_s};
    // undef when unparsable; zero would read as never paying
    if (const auto limit = parse_decimal_e9(wire.funding_rate_limit); limit.has_value() && limit->raw > 0) {
        perp.funding_cap = *limit;
        perp.funding_floor = FixedE9::from_raw(-limit->raw);
    }
    return perp;
}

namespace {

constexpr std::string_view k_option_quote = "_USDT";

[[nodiscard]] std::optional<FixedE9> quoted_vol(std::string_view text) {
    const auto vol = to_e9(text);
    return vol.has_value() && vol->raw > 0 ? vol : std::nullopt;
}

[[nodiscard]] std::optional<InstrumentId> option_id(std::string_view contract, const exec::InstrumentTable& table) {
    const auto symbol = option_symbol(contract);
    const auto* instrument = symbol.has_value() ? table.find(symbol->view()) : nullptr;
    if (instrument == nullptr || !std::holds_alternative<OptionInstrument>(*instrument)) {
        return std::nullopt;
    }
    return base_of(*instrument).id;
}

} // namespace

std::optional<SymbolText> option_symbol(std::string_view wire_name) noexcept {
    const auto dash = wire_name.find('-');
    if (dash == std::string_view::npos || !wire_name.substr(0, dash).ends_with(k_option_quote)) {
        return std::nullopt;
    }
    std::array<char, 32> buffer{};
    const auto head = dash - k_option_quote.size();
    const auto tail = wire_name.substr(dash);
    if (head + tail.size() > buffer.size()) {
        return std::nullopt;
    }
    wire_name.copy(buffer.data(), head);
    tail.copy(buffer.data() + head, tail.size());
    const auto held = SymbolText::from(std::string_view{buffer.data(), head + tail.size()});
    return held.has_value() ? std::optional{*held} : std::nullopt;
}

std::string option_underlying(std::string_view symbol) {
    std::string out{symbol.substr(0, symbol.find('-'))};
    out.append(k_option_quote);
    return out;
}

std::optional<Instrument> map_option_contract(const WireOptionContract& wire) {
    const auto symbol = option_symbol(wire.name);
    const auto tick = parse_decimal_e9(wire.order_price_round);
    const auto multiplier = parse_decimal_e9(wire.multiplier);
    if (!wire.is_active || !symbol.has_value() || !tick.has_value() || !multiplier.has_value() || tick->raw <= 0 ||
        multiplier->raw <= 0 || wire.expiration_s == 0) {
        return std::nullopt;
    }
    const auto strike = exact_steps(wire.strike_price, *tick);
    if (!strike.has_value() || *strike <= 0) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Gate, MarketType::Option, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.asset_class = AssetClass::Crypto;
    base.tick_size = *tick;
    base.lot_size = FixedE9::from_raw(k_e9);
    base.contract_value = *multiplier;
    base.kind = ContractKind::Linear;
    // the premium is usdt per one underlying unit, sizes are contracts of `multiplier` units
    return OptionInstrument{.base = base,
                            .strike = PriceTicks::from_raw(*strike),
                            .expiry = s_to_unix(wire.expiration_s),
                            .put_call = wire.is_call ? PutCall::Call : PutCall::Put,
                            .premium = OptionUnit::Quote,
                            .greeks = OptionUnit::Quote};
}

std::optional<OptionGreeks>
map_option_greeks(const WireOptionTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto id = option_id(wire.contract, table);
    if (!id.has_value()) {
        return std::nullopt;
    }
    const options::VenueGreeks venue{.delta = to_e9(wire.delta),
                                     .gamma = to_e9(wire.gamma),
                                     .vega = to_e9(wire.vega),
                                     .theta = to_e9(wire.theta),
                                     .rho = to_e9(wire.rho),
                                     .mark_iv = quoted_vol(wire.mark_iv),
                                     .bid_iv = quoted_vol(wire.bid_iv),
                                     .ask_iv = quoted_vol(wire.ask_iv),
                                     .underlying_price = to_e9(wire.underlying_price),
                                     .mark_price = to_e9(wire.mark_price),
                                     .index_price = to_e9(wire.index_price)};
    const auto ts_event = wire.ts_s == 0 ? ts_init : s_to_unix(wire.ts_s);
    return options::normalise(*id, venue, options::Convention{}, ts_event, ts_init);
}

void blank_empty_sides(Bbo& bbo) noexcept {
    if (bbo.bid_qty.raw == 0) {
        bbo.bid = PriceTicks{};
    }
    if (bbo.ask_qty.raw == 0) {
        bbo.ask = PriceTicks{};
    }
}

std::optional<Bbo>
map_option_quote(const WireOptionTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto id = option_id(wire.contract, table);
    if (!id.has_value()) {
        return std::nullopt;
    }
    const auto& base = base_of(*table.find(*id));
    const auto bid = to_ticks(wire.bid_price, base);
    const auto ask = to_ticks(wire.ask_price, base);
    const auto bid_qty = to_lots(wire.bid_qty, base);
    const auto ask_qty = to_lots(wire.ask_qty, base);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = *id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    blank_empty_sides(out);
    out.ts_event = s_to_unix(wire.ts_s);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_currency_pair(const WireCurrencyPair& wire) {
    // buyable and sellable are one-sided listings
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
    base.asset_class = AssetClass::Crypto;
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

// fees truncate at 1e-9: gate pads futures fees past FixedE9 precision
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

// only a t- prefixed id that fits is ours; anything else stays unattributed
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
        // the user trade that emptied leaves already reported this fill
        return std::nullopt;
    case WireOrderState::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = wire.sell ? Side::Sell : Side::Buy;
    // futures has no order type: price 0 with ioc is a market order
    out.order_type = (wire.order_type == "market" || (wire.price == "0" && wire.tif == "ioc")) ? OrderType::Market
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

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire, const exec::InstrumentTable& table) {
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

// a wallet figure; gate sends twelve fractional digits, so it truncates
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
    // each field stays undef when omitted or unreadable
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

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.contract);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    // the magnitude converts and the sign is reapplied
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
