#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/nado/map.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>

namespace raikiri::venues::nado {

namespace {

constexpr std::size_t k_x18_to_e9_digits = 9;
constexpr std::size_t k_max_e9_digits = 18;
constexpr std::int64_t k_hours_per_day = 24;
constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;

std::optional<std::int64_t> steps_of(std::string_view x18, FixedE9 step) noexcept {
    const auto value = x18_to_e9(x18, /*exact=*/true);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::optional<FixedE9> x18_to_e9(std::string_view text, bool exact) noexcept {
    const bool negative = text.starts_with('-');
    const auto digits = negative ? text.substr(1) : text;
    if (digits.empty() || !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    const auto cut = digits.size() > k_x18_to_e9_digits ? digits.size() - k_x18_to_e9_digits : 0;
    const auto head = digits.substr(0, cut);
    const auto tail = digits.substr(cut);
    if (exact && std::ranges::any_of(tail, [](char c) { return c != '0'; })) {
        return std::nullopt;
    }
    if (head.size() > k_max_e9_digits) {
        return std::nullopt;
    }
    const auto raw = head.empty() ? std::int64_t{0} : static_cast<std::int64_t>(wire::digits_to_u64(head));
    return FixedE9::from_raw(negative ? -raw : raw);
}

std::optional<Instrument> map_instrument(const WireSymbol& wire, MarketType market) {
    if (market != MarketType::Perp || wire.type != "perp" || wire.status == "not_tradable") {
        return std::nullopt;
    }
    const auto tick = x18_to_e9(wire.tick_x18, /*exact=*/true);
    const auto lot = x18_to_e9(wire.lot_x18, /*exact=*/true);
    const auto symbol = SymbolText::from(wire.symbol);
    const auto base_name = wire.symbol.substr(0, wire.symbol.find('-'));
    // collateral is usdt0, tether bridged onto ink
    const auto local = canonical_symbol(base_name, "USDT");
    if (!tick.has_value() || !lot.has_value() || tick->raw <= 0 || lot->raw <= 0 || !symbol.has_value() ||
        !local.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Nado, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    if (const auto min = x18_to_e9(wire.min_notional_x18, /*exact=*/false); min.has_value()) {
        base.min_notional = *min;
    }
    if (const auto fee = x18_to_e9(wire.maker_fee_x18, /*exact=*/false); fee.has_value()) {
        base.maker_fee = *fee;
    }
    if (const auto fee = x18_to_e9(wire.taker_fee_x18, /*exact=*/false); fee.has_value()) {
        base.taker_fee = *fee;
    }
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = AssetClass::Crypto;
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto bid = steps_of(wire.bid_price, base.tick_size);
    const auto ask = steps_of(wire.ask_price, base.tick_size);
    const auto bid_qty = steps_of(wire.bid_qty, base.lot_size);
    const auto ask_qty = steps_of(wire.ask_qty, base.lot_size);
    // an empty side is sent as a zero price
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value() || *bid <= 0 ||
        *ask <= 0 || *bid_qty <= 0 || *ask_qty <= 0) {
        return std::nullopt;
    }
    return Bbo{.instrument = base.id,
               .bid = PriceTicks::from_raw(*bid),
               .ask = PriceTicks::from_raw(*ask),
               .bid_qty = QtyLots::from_raw(*bid_qty),
               .ask_qty = QtyLots::from_raw(*ask_qty),
               .ts_event = UnixNanos{wire.ts_ns},
               .ts_init = ts_init};
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *price <= 0 || *qty <= 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.taker_bought ? Side::Buy : Side::Sell;
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, PriceTicks mark, PriceTicks index, UnixNanos ts_init) {
    const auto daily = x18_to_e9(wire.rate_x18, /*exact=*/false);
    if (!daily.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = FixedE9::from_raw(daily->raw / k_hours_per_day);
    out.predicted_rate = FixedE9::undef();
    out.mark_price = mark;
    out.index_price = index;
    // settled on the hour; the rate is restated about every 20 s
    if (wire.update_s > 0) {
        const auto next_s = (wire.update_s / k_funding_interval_s + 1) * k_funding_interval_s;
        out.next_funding_time = UnixNanos{next_s * k_ns_per_s};
    }
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_event = UnixNanos{wire.ts_ns};
    out.ts_init = ts_init;
    return out;
}

std::optional<PriceTicks> map_price(std::string_view decimal, const InstrumentBase& base) noexcept {
    const auto value = parse_rate_e9(decimal);
    if (!value.has_value() || value->raw <= 0 || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(value->raw / base.tick_size.raw);
}

std::optional<TickerStats> map_ticker_stats(const WireContract& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto quote = parse_turnover_e6(wire.quote_volume);
    if (!quote.has_value()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_quote = *quote;
    out.volume_24h_base = parse_rate_e9(wire.base_volume).value_or(FixedE9::from_raw(0));
    out.open_interest = parse_rate_e9(wire.open_interest).value_or(FixedE9::undef());
    out.ts_event = ts_init;
    out.ts_init = ts_init;
    return out;
}

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    chained_ = false;
    snapshot_ts_ = 0;
    last_max_ts_ = 0;
}

bool Book::apply_side(std::span<const WireLevel> levels, book::Ladder& side) const {
    for (const auto& wire : levels) {
        const auto price = steps_of(wire.price, tick_size_);
        const auto qty = steps_of(wire.qty, lot_size_);
        if (!price.has_value() || !qty.has_value() || *qty < 0) {
            return false;
        }
        if (*qty == 0) {
            side.remove(PriceTicks::from_raw(*price));
            continue;
        }
        side.upsert({PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)});
    }
    return true;
}

BookResult Book::resync(const WireDepth& snapshot) {
    invalidate();
    if (!apply_side(snapshot.bids, ladders_.bids()) || !apply_side(snapshot.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    snapshot_ts_ = snapshot.max_ts;
    last_max_ts_ = snapshot.max_ts;
    synced_ = true;
    return BookResult::Resynced;
}

BookResult Book::apply(const WireDepth& push) {
    if (!synced_) {
        return BookResult::Ignored;
    }
    if (!chained_) {
        if (push.max_ts <= snapshot_ts_) {
            return BookResult::Stale;
        }
        // a push that starts after the snapshot means one between them was never seen
        if (push.last_max_ts > snapshot_ts_) {
            invalidate();
            return BookResult::Gap;
        }
    } else if (push.last_max_ts != last_max_ts_) {
        invalidate();
        return BookResult::Gap;
    }
    if (!apply_side(push.bids, ladders_.bids()) || !apply_side(push.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    last_max_ts_ = push.max_ts;
    chained_ = true;
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_max_ts_, UnixNanos{last_max_ts_}, ts_init);
}

} // namespace raikiri::venues::nado
