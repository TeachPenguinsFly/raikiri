#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/kucoin/map.hpp>

#include <array>
#include <charconv>
#include <optional>
#include <variant>

namespace raikiri::venues::kucoin {

namespace {

// 10^0 .. 10^18; 10^19 does not fit int64, so an exponent past this range is
// refused rather than saturated.
constexpr std::array<std::int64_t, 19> k_pow10 = {1LL,
                                                  10LL,
                                                  100LL,
                                                  1'000LL,
                                                  10'000LL,
                                                  100'000LL,
                                                  1'000'000LL,
                                                  10'000'000LL,
                                                  100'000'000LL,
                                                  1'000'000'000LL,
                                                  10'000'000'000LL,
                                                  100'000'000'000LL,
                                                  1'000'000'000'000LL,
                                                  10'000'000'000'000LL,
                                                  100'000'000'000'000LL,
                                                  1'000'000'000'000'000LL,
                                                  10'000'000'000'000'000LL,
                                                  100'000'000'000'000'000LL,
                                                  1'000'000'000'000'000'000LL};

// Exact conversion against the instrument's own step. A value that does not
// divide evenly means KuCoin changed an instrument's precision, so the message
// is dropped rather than rounded — rounding is how an order ends up one tick
// away from the intended price.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(text);
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
    return parse_number_e9(text);
}

// A wallet figure, and the reader above is the wrong one for it: KuCoin
// renders balances as Java doubles and sends ten fractional digits for USDT,
// which the exact reader refuses outright — showing as an account that holds
// nothing rather than as a number this cannot represent.
std::optional<FixedE9> to_amount(std::string_view text) {
    return text.empty() ? std::nullopt : raikiri::parse_amount_e9(text);
}

bool copy_id(std::string_view text, ExchangeOrderId& out) {
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

// Our own ULID coming back from the venue. A value that does not fit the field
// cannot be one of ours, so the event stays unattributed rather than being
// matched to a truncated lookalike.
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

Side side_from(std::string_view text) noexcept {
    return text == "sell" ? Side::Sell : Side::Buy;
}

Liquidity liquidity_from(std::string_view text) noexcept {
    if (text == "maker") {
        return Liquidity::Maker;
    }
    if (text == "taker") {
        return Liquidity::Taker;
    }
    return Liquidity::Unknown; // spot omits it entirely
}

} // namespace

std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int32_t exponent = 0;
    if (const auto marker = text.find_first_of("eE"); marker != std::string_view::npos) {
        auto digits = text;
        digits.remove_prefix(marker + 1);
        text.remove_suffix(text.size() - marker);
        bool negative = false;
        if (!digits.empty() && (digits.front() == '+' || digits.front() == '-')) {
            negative = digits.front() == '-';
            digits.remove_prefix(1);
        }
        const auto* const end = digits.data() + digits.size();
        const auto [stop, ec] = std::from_chars(digits.data(), end, exponent);
        if (ec != std::errc{} || stop != end) {
            return std::nullopt;
        }
        if (negative) {
            exponent = -exponent;
        }
    }
    const auto mantissa = parse_decimal_e9(text);
    if (!mantissa.has_value()) {
        return std::nullopt;
    }
    if (exponent >= 0) {
        if (static_cast<std::size_t>(exponent) >= k_pow10.size()) {
            return std::nullopt;
        }
        std::int64_t out = 0;
        if (__builtin_mul_overflow(
                mantissa->raw, k_pow10[static_cast<std::size_t>(exponent)], &out)) {
            return std::nullopt;
        }
        return FixedE9::from_raw(out);
    }
    const auto index = static_cast<std::size_t>(-exponent);
    if (index >= k_pow10.size()) {
        return std::nullopt;
    }
    const auto divisor = k_pow10[index];
    if (mantissa->raw % divisor != 0) {
        return std::nullopt;
    }
    return FixedE9::from_raw(mantissa->raw / divisor);
}

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * 1'000'000ULL};
}

UnixNanos ns_to_unix(std::uint64_t ns, UnixNanos fallback) noexcept {
    return ns == 0 ? fallback : UnixNanos{ns};
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

namespace {

// Levels stop at the domain's ceiling rather than failing: a truncated book is
// still a correct top-of-book, and the count says how much of it is real. A
// level that will not convert voids the side, because a book with a hole in it
// is a guess.
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
            return 0;
        }
        into[count] = BookLevel{.price = *price, .qty = *qty};
        ++count;
    }
    return count;
}

} // namespace

std::optional<BookSnapshot>
map_book(const WireBookSnapshot& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
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
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
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
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireContract& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    if (wire.volume_24h.empty() && wire.turnover_24h.empty()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    if (const auto base = to_e9(wire.volume_24h); base.has_value()) {
        out.volume_24h_base = *base;
    }
    if (const auto quote = parse_turnover_e6(wire.turnover_24h); quote.has_value()) {
        out.volume_24h_quote = *quote;
    }
    // The catalogue states no clock of its own, so the read is its own stamp.
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireSpotTicker& wire,
                                            const exec::InstrumentTable& table,
                                            UnixNanos ts_init) {
    // Java doubles past nine decimals ("3093.1776802805860382"); an exact read refuses them.
    const auto base = wire.vol.empty() ? std::nullopt : parse_amount_e9(wire.vol);
    const auto quote = wire.vol_value.empty() ? std::nullopt : parse_turnover_e6(wire.vol_value);
    if (!base.has_value() && !quote.has_value()) {
        return std::nullopt;
    }
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base_of(*instrument).id;
    if (base.has_value()) {
        out.volume_24h_base = *base;
    }
    if (quote.has_value()) {
        out.volume_24h_quote = *quote;
    }
    out.ts_event = wire.ts_ms == 0 ? ts_init : UnixNanos{wire.ts_ms * 1'000'000ULL};
    out.ts_init = ts_init;
    return out;
}

// One record out of everything both subjects have said. Fields nobody has
// stated yet stay UNDEF: the reader checks for that, and a zero would be a
// number this process invented.
[[nodiscard]] FundingUpdate merged(const Instrument& instrument,
                                   const FundingMemo& memo,
                                   UnixNanos ts_event,
                                   UnixNanos ts_init) {
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

std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                         std::string_view symbol,
                                         const exec::InstrumentTable& table,
                                         FundingMemo& memo,
                                         UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    memo.rate = *rate;
    if (const auto predicted = to_e9(wire.predicted_rate); predicted.has_value()) {
        memo.predicted = *predicted;
    }
    const auto* perp = std::get_if<PerpInstrument>(instrument);
    const std::uint64_t interval_ms =
        perp == nullptr ? 0 : static_cast<std::uint64_t>(perp->funding_interval_s) * 1000ULL;
    memo.interval_s = perp == nullptr ? 0 : perp->funding_interval_s;
    // Only the REST form states the next settlement.
    memo.next_funding = wire.next_funding_ms != 0 ? ms_to_unix(wire.next_funding_ms, UnixNanos{0})
                        : (interval_ms != 0 && wire.ts_ms != 0)
                            ? ms_to_unix((wire.ts_ms / interval_ms + 1) * interval_ms, UnixNanos{0})
                            : UnixNanos{0};
    return merged(*instrument, memo, ms_to_unix(wire.ts_ms, ts_init), ts_init);
}

std::optional<FundingUpdate> map_mark_price(const WireMarkPrice& wire,
                                            const exec::InstrumentTable& table,
                                            FundingMemo& memo,
                                            UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    // NOT the exact reader every other price goes through. KuCoin quotes the
    // mark on indexPriceTickSize (0.01 on XBTUSDTM) while the contract trades
    // on 0.1, so the exact reader refuses every live mark and the column stays
    // empty forever. A mark is a reference figure and is never sent back as an
    // order price, so the tail below the contract's tick is dropped rather
    // than the whole number.
    const auto& base = base_of(*instrument);
    const auto mark = parse_number_e9(wire.mark_price);
    if (!mark.has_value() || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    memo.mark = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    return merged(*instrument, memo, ms_to_unix(wire.ts_ms, ts_init), ts_init);
}

namespace {

[[nodiscard]] AssetClass asset_class_of(std::string_view asset_class) noexcept {
    if (asset_class == "CRYPTO") {
        return AssetClass::Crypto;
    }
    if (asset_class == "STOCK") {
        return AssetClass::Equity;
    }
    if (asset_class == "COMMODITY" || asset_class == "METAL") {
        return AssetClass::Commodity;
    }
    return AssetClass::Unknown;
}

} // namespace

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    switch (market) {
    case MarketType::Perp:
        break;
    case MarketType::Spot:
        return std::nullopt; // the futures host serves no spot; map_spot_symbol does
    case MarketType::Future:
        // KuCoin lists dated contracts on this endpoint, every one seen inverse.
        return std::nullopt;
    case MarketType::Option:
        return std::nullopt; // KuCoin lists no options on the APIs in scope
    }
    if (wire.status != "Open") {
        return std::nullopt;
    }
    if (wire.type != "FFWCSX" || wire.is_inverse) {
        return std::nullopt;
    }

    const auto tick = parse_number_e9(wire.tick_size);
    const auto lot = parse_number_e9(wire.lot_size);
    const auto multiplier = parse_number_e9(wire.multiplier);
    if (!tick.has_value() || !lot.has_value() || !multiplier.has_value() || tick->raw <= 0 ||
        lot->raw <= 0 || multiplier->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Kucoin, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    // The catalogue names both halves, which is the only place XBTUSDTM says
    // what it trades. A row whose pair KuCoin did not state is DROPPED rather
    // than carried under the exchange symbol: an instrument nothing can group
    // shows in a cross-venue view as a market of its own.
    const auto canonical = canonical_symbol(wire.base_currency, wire.quote_currency);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.asset_class = asset_class_of(wire.asset_class);
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *multiplier;
    base.kind = ContractKind::Linear;
    return PerpInstrument{base, static_cast<std::uint32_t>(wire.funding_granularity_ms / 1000)};
}

std::optional<OrderEvent> map_order_change(const WireOrderChange& wire,
                                           const exec::InstrumentTable& table,
                                           UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);

    OrderEventType type{};
    switch (wire.type) {
    case OrderChangeType::Open:
        type = OrderEventType::Accepted;
        break;
    case OrderChangeType::Match:
        type = OrderEventType::Filled;
        break;
    case OrderChangeType::Canceled:
        type = OrderEventType::Canceled;
        break;
    case OrderChangeType::Update:
        type = OrderEventType::Updated;
        break;
    case OrderChangeType::Filled:
        // The trades already arrived as `match`; this is the venue closing the
        // order, and emitting it as a fill counts the last one twice.
        return std::nullopt;
    case OrderChangeType::Unknown:
        return std::nullopt;
    }

    OrderEvent out{};
    out.type = type;
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.order_type = wire.order_type == "market" ? OrderType::Market : OrderType::Limit;
    if (!copy_client_id(wire.client_oid, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }

    if (wire.type == OrderChangeType::Match) {
        const auto price = to_ticks(wire.match_price, base);
        const auto qty = to_lots(wire.match_size, base);
        if (!price.has_value() || !qty.has_value()) {
            return std::nullopt; // a fill we cannot scale is a schema event
        }
        out.price = *price;
        out.qty = *qty;
        out.liquidity = liquidity_from(wire.liquidity);
        if (!wire.trade_id.empty()) {
            const auto exec_id = ExecId::from(wire.trade_id);
            if (!exec_id.has_value()) {
                return std::nullopt; // a dedupe key we cannot store is not usable
            }
            out.exec_id = *exec_id;
        }
        // The frame names the fee TYPE and never the amount, so the charge for
        // this fill is only knowable from the REST pull. Zero is what the
        // ledger can arithmetic on; undef would poison it.
        out.fee = FixedE9::from_raw(0);
    } else {
        if (const auto price = to_ticks(wire.price, base); price.has_value()) {
            out.price = *price;
        }
        if (const auto qty = to_lots(wire.size, base); qty.has_value()) {
            out.qty = *qty;
        }
    }
    if (const auto leaves = to_lots(wire.remain_size, base); leaves.has_value()) {
        out.leaves_qty = *leaves;
    }
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                      const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = to_lots(wire.size, base);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    const auto dealt = to_lots(wire.deal_size, base);

    exec::OrderStatusReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    if (!copy_client_id(wire.client_oid, out.exchange_client_order_id) ||
        !copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.status =
        dealt.has_value() && dealt->raw > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
    if (const auto price = to_ticks(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    // Spot states what is left; futures states what was done and leaves the
    // subtraction here.
    if (const auto leaves = to_lots(wire.remain_size, base); leaves.has_value()) {
        out.leaves = *leaves;
    } else if (dealt.has_value()) {
        out.leaves = QtyLots::from_raw(qty->raw - dealt->raw);
    } else {
        return std::nullopt; // neither form of "how much is left" was readable
    }
    out.ts_venue = ms_to_unix(wire.updated_ms, UnixNanos{});
    return out;
}

std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = to_ticks(wire.price, base);
    const auto qty = to_lots(wire.size, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }

    exec::FillReport out{};
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.liquidity = liquidity_from(wire.liquidity);
    if (!copy_id(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    if (!wire.trade_id.empty()) {
        const auto exec_id = ExecId::from(wire.trade_id);
        if (!exec_id.has_value()) {
            return std::nullopt;
        }
        out.exec_id = *exec_id;
    }
    out.price = *price;
    out.qty = *qty;
    // Neither host names the clientOid on a fill, so exchange_client_order_id
    // stays empty and the fill is attributed through its order id alone.
    if (const auto fee = to_e9(wire.fee); fee.has_value()) {
        out.fee = *fee;
    } else {
        out.fee = FixedE9::from_raw(0);
    }
    out.ts_venue = ms_to_unix(wire.created_ms, UnixNanos{});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                 const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    // Signed already: there is no side field that could disagree with it.
    const auto qty = to_lots(wire.current_qty, base);
    if (!qty.has_value()) {
        return std::nullopt;
    }

    exec::PositionReport out{};
    out.instrument = base.id;
    out.net_qty = *qty;
    if (const auto avg = to_ticks(wire.avg_entry_price, base); avg.has_value()) {
        out.avg_entry = *avg;
    }
    out.ts_venue = ms_to_unix(wire.ts_ms, UnixNanos{});
    return out;
}

std::optional<BalanceReport> map_account_overview(const WireAccountOverview& wire,
                                                  UnixNanos ts_init) {
    const auto asset = to_asset_code(wire.currency);
    if (!asset.has_value()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    // Each field left UNDEF where KuCoin omitted it or sent something this
    // cannot read. A zero would claim the account holds nothing.
    if (const auto total = to_amount(wire.margin_balance); total.has_value()) {
        out.total = *total;
    }
    if (const auto available = to_amount(wire.available_balance); available.has_value()) {
        out.available = *available;
    }
    if (const auto unrealized = to_amount(wire.unrealised_pnl); unrealized.has_value()) {
        out.unrealized = *unrealized;
    }
    // The overview carries no clock of its own.
    out.ts_venue = ts_init;
    return out;
}

std::optional<Instrument> map_spot_symbol(const WireSpotSymbol& wire) {
    if (!wire.enable_trading) {
        return std::nullopt;
    }
    const auto tick = parse_number_e9(wire.price_increment);
    const auto lot = parse_number_e9(wire.base_increment);
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.symbol);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Kucoin, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    // The spot symbol carries the pair itself — `BTC-USDT` — where the futures
    // one does not. A row whose pair KuCoin did not state is DROPPED rather
    // than carried under the exchange symbol: an instrument nothing can group
    // shows in a cross-venue view as a market of its own.
    const auto pair = split_symbol(wire.symbol, '-');
    if (!pair.has_value()) {
        return std::nullopt;
    }
    const auto canonical = canonical_symbol(pair->first.view(), pair->second.view());
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    base.local_symbol = *canonical;
    base.asset_class = AssetClass::Crypto;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min_funds = to_e9(wire.min_funds); min_funds.has_value()) {
        base.min_notional = *min_funds;
    }
    // A spot lot IS one base unit; there is no contract in between.
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    return SpotInstrument{base};
}

} // namespace raikiri::venues::kucoin
