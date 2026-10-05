#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/options/convention.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/positions.hpp>

#include <optional>
#include <string_view>

namespace raikiri::venues::okx {

namespace {

// a remainder means the instrument's precision changed, so the message is dropped
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

// okx pads rates to 16 fractional digits; parse_rate_e9 truncates the dust below 1e-9
std::optional<FixedE9> rate_of(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    return parse_rate_e9(text);
}

// turnover and wallet figures are truncated below 1e-9, since okx pads some past nine digits
std::optional<FixedE9> to_amount(std::string_view text) {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

// okx pads inverse fees past 1e-9, so they are truncated; never use this for a price or size.
// sign flipped: okx writes a charge as negative, the ledger counts it positive
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

// leaves is sz minus accFillSz; nullopt unless both convert exactly
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

// a value that does not fit cannot be one of our ids
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

// ctMult must be a whole number
std::optional<std::int64_t> whole_number(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw <= 0 || value->raw % k_e9 != 0) {
        return std::nullopt;
    }
    return value->raw / k_e9;
}

// swap rows leave baseCcy and quoteCcy empty, so the kind suffix is stripped from instId
std::optional<SymbolText> canonical_of(const WireInstrument& wire) {
    if (!wire.base_ccy.empty() && !wire.quote_ccy.empty()) {
        return canonical_symbol(wire.base_ccy, wire.quote_ccy);
    }
    constexpr std::string_view k_swap_suffix = "-SWAP";
    auto pair_text = wire.inst_id;
    if (pair_text.ends_with(k_swap_suffix)) {
        pair_text.remove_suffix(k_swap_suffix.size());
    }
    const auto pair = split_symbol(pair_text, '-');
    if (!pair.has_value()) {
        return std::nullopt;
    }
    return canonical_symbol(pair->first.view(), pair->second.view());
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo> map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
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
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

// fields nobody has stated stay undef
[[nodiscard]] FundingUpdate
merged(const Instrument& instrument, const FundingMemo& memo, UnixNanos ts_event, UnixNanos ts_init) {
    FundingUpdate out{};
    out.instrument = base_of(instrument).id;
    out.rate = memo.rate;
    out.predicted_rate = memo.predicted;
    out.mark_price = memo.mark;
    out.next_funding_time = memo.next_funding;
    out.interval_s = memo.interval_s;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, FundingMemo& memo, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = rate_of(wire.funding_rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    memo.rate = *rate;
    if (const auto predicted = rate_of(wire.next_funding_rate); predicted.has_value()) {
        memo.predicted = *predicted;
    }
    memo.next_funding = ms_to_unix(wire.funding_ms, UnixNanos{0});
    if (wire.next_funding_ms > wire.funding_ms && wire.funding_ms != 0) {
        memo.interval_s = static_cast<std::uint32_t>((wire.next_funding_ms - wire.funding_ms) / 1000);
    }
    return merged(*instrument, memo, ms_to_unix(wire.ts_ms, UnixNanos{}), ts_init);
}

std::optional<FundingUpdate>
map_mark_price(const WireMarkPrice& wire, const exec::InstrumentTable& table, FundingMemo& memo, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto mark = to_ticks(wire.mark_price, base_of(*instrument));
    if (!mark.has_value()) {
        return std::nullopt; // a mark this venue's tick cannot express is no mark
    }
    memo.mark = *mark;
    return merged(*instrument, memo, ms_to_unix(wire.ts_ms, UnixNanos{}), ts_init);
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    // both columns empty means no turnover on this frame
    if (wire.vol_24h.empty() && wire.vol_ccy_24h.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    // spot: vol24h base, volCcy24h quote; derivatives: volCcy24h base, vol24h contracts
    const bool is_spot = wire.inst_type == "SPOT" || wire.inst_type == "MARGIN";
    const auto& base = base_of(*instrument);

    TickerStats out{};
    out.instrument = base.id;
    const auto coins = to_amount(is_spot ? wire.vol_24h : wire.vol_ccy_24h);
    if (coins.has_value()) {
        out.volume_24h_base = *coins;
    }
    if (is_spot) {
        if (const auto quote = parse_turnover_e6(wire.vol_ccy_24h); quote.has_value()) {
            out.volume_24h_quote = *quote;
        }
    } else if (base.kind == ContractKind::Inverse) {
        // an inverse contract is a fixed dollar amount, so the count is the turnover
        if (const auto contracts = to_amount(wire.vol_24h); contracts.has_value()) {
            out.volume_24h_quote = turnover_e6(*contracts, base.contract_value);
        }
    } else if (const auto last = to_amount(wire.last); last.has_value()) {
        // no derivative quote column: coins at the last print, priced per contract when e9 overflows
        if (coins.has_value()) {
            out.volume_24h_quote = turnover_e6(*coins, *last);
        } else if (const auto contracts = to_amount(wire.vol_24h); contracts.has_value()) {
            out.volume_24h_quote = turnover_e6(*contracts, turnover_e9(base.contract_value, *last));
        }
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

namespace {

[[nodiscard]] std::optional<FixedE9> quoted_vol(std::string_view text) {
    const auto vol = rate_of(text);
    return vol.has_value() && vol->raw > 0 ? vol : std::nullopt;
}

[[nodiscard]] std::optional<FixedE9> remembered(FixedE9 value) {
    return value.is_undef() ? std::nullopt : std::optional{value};
}

} // namespace

std::optional<OptionGreeks> map_option_greeks(const WireOptSummary& wire,
                                              const exec::InstrumentTable& table,
                                              const OptionMemo& memo,
                                              UnixNanos ts_init) {
    const auto* instrument = table.find(wire.inst_id);
    if (instrument == nullptr || !std::holds_alternative<OptionInstrument>(*instrument)) {
        return std::nullopt;
    }
    const options::VenueGreeks venue{.delta = rate_of(wire.delta_bs),
                                     .gamma = rate_of(wire.gamma_bs),
                                     .vega = rate_of(wire.vega_bs),
                                     .theta = rate_of(wire.theta_bs),
                                     .mark_iv = quoted_vol(wire.mark_vol),
                                     .bid_iv = quoted_vol(wire.bid_vol),
                                     .ask_iv = quoted_vol(wire.ask_vol),
                                     .underlying_price = rate_of(wire.forward),
                                     .mark_price = remembered(memo.mark),
                                     .index_price = remembered(memo.index)};
    return options::normalise(
        base_of(*instrument).id, venue, options::Convention{}, ms_to_unix(wire.ts_ms, ts_init), ts_init);
}

std::string_view option_family(std::string_view inst_id) noexcept {
    const auto first = inst_id.find('-');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto second = inst_id.find('-', first + 1);
    return second == std::string_view::npos ? std::string_view{} : inst_id.substr(0, second);
}

std::string_view option_index(std::string_view family) noexcept {
    return family.substr(0, family.find('_'));
}

Side side_from(std::string_view text) noexcept {
    return text == "sell" ? Side::Sell : Side::Buy;
}

std::optional<OrderEvent> map_order_fill(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    // a fill without computable leaves is dropped; reconciliation corrects the order
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
        // the order is still working, so a refused amend is ModifyRejected
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
            // cancelSource 13 is a fok that could not fill whole and 14 an ioc remainder
            type = (wire.cancel_source == "13" || wire.cancel_source == "14") ? OrderEventType::Expired
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

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire, const exec::InstrumentTable& table) {
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

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table) {
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
    // net mode signs pos; long/short mode returns one unsigned row a side
    out.net_qty = wire.pos_side == "short" ? QtyLots::from_raw(size->raw < 0 ? size->raw : -size->raw) : *size;
    if (const auto avg = to_ticks(wire.avg_px, base); avg.has_value()) {
        out.avg_entry = *avg;
    }
    out.ts_venue = ms_to_unix(wire.u_ms, UnixNanos{});
    return out;
}

std::optional<BalanceReport> map_balance(const WireCurrencyBalance& wire, AccountMode mode, UnixNanos fallback) {
    const auto asset = to_asset_code(wire.ccy);
    if (!asset.has_value() || asset->empty()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    // fields stay undef where okx sent an empty string
    if (const auto total = to_amount(wire.cash_bal); total.has_value()) {
        out.total = *total;
    }
    if (const auto unrealized = to_amount(wire.upl); unrealized.has_value()) {
        out.unrealized = *unrealized;
    }
    // published only in spot and futures mode, where availBal is the currency's own free amount;
    // the margin modes check account-wide equity, so availBal would overstate
    if (mode == AccountMode::Spot || mode == AccountMode::Futures) {
        if (const auto available = to_amount(wire.avail_bal); available.has_value()) {
            out.available = *available;
        }
    }
    out.ts_venue = ms_to_unix(wire.u_time_ms, fallback);
    return out;
}

namespace {

[[nodiscard]] AssetClass asset_class_of(std::string_view inst_category) noexcept {
    if (inst_category == "1") {
        return AssetClass::Crypto;
    }
    if (inst_category == "3") {
        return AssetClass::Equity;
    }
    if (inst_category == "4") {
        return AssetClass::Commodity;
    }
    if (inst_category == "5") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

std::optional<Instrument> map_option(const WireInstrument& wire) {
    const auto tick = parse_decimal_e9(wire.tick_size);
    const auto lot = parse_decimal_e9(wire.lot_size);
    const auto ct_val = parse_decimal_e9(wire.ct_val);
    const auto ct_mult = parse_decimal_e9(wire.ct_mult);
    const auto symbol = SymbolText::from(wire.inst_id);
    if (!tick.has_value() || !lot.has_value() || !ct_val.has_value() || !ct_mult.has_value() || !symbol.has_value() ||
        tick->raw <= 0 || lot->raw <= 0 || ct_val->raw <= 0 || ct_mult->raw <= 0 || wire.expiry_ms == 0 ||
        (wire.opt_type != "C" && wire.opt_type != "P") || (wire.ct_type != "inverse" && wire.ct_type != "linear")) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Okx, MarketType::Option, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.asset_class = asset_class_of(wire.inst_category);
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.kind = wire.ct_type == "inverse" ? ContractKind::Inverse : ContractKind::Linear;
    base.contract_value = FixedE9::from_raw(ct_val->raw * ct_mult->raw / k_e9);
    const auto strike = exact_steps(wire.strike, base.tick_size);
    if (!strike.has_value() || *strike <= 0 || base.contract_value.raw <= 0) {
        return std::nullopt;
    }
    // an inverse family is premium in the coin, a _UM one in usd; the BS greeks are usd on both
    return OptionInstrument{.base = base,
                            .strike = PriceTicks::from_raw(*strike),
                            .expiry = ms_to_unix(wire.expiry_ms, UnixNanos{0}),
                            .put_call = wire.opt_type == "P" ? PutCall::Put : PutCall::Call,
                            .premium = base.kind == ContractKind::Inverse ? OptionUnit::Base : OptionUnit::Quote,
                            .greeks = OptionUnit::Quote};
}

} // namespace

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (wire.state != "live") {
        return std::nullopt;
    }
    if (market == MarketType::Option) {
        return wire.inst_type == "OPTION" ? map_option(wire) : std::nullopt;
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
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Okx, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    // a row whose pair okx did not state is dropped
    const auto canonical = canonical_of(wire);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.asset_class = asset_class_of(wire.inst_category);
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
    // ct_type alone tells linear from inverse; a row without it is refused
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
