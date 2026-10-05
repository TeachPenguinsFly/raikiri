#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/derive/map.hpp>

#include <array>
#include <span>

namespace raikiri::venues::derive {

namespace {

constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;
constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;

std::optional<FixedE9> positive(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() && value->raw > 0 ? std::optional{*value} : std::nullopt;
}

// prices and sizes sit on the grid; anything off it is refused, never rounded
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

// greeks, vols and marks carry up to 18 digits; analytics keep nine
std::optional<FixedE9> figure(std::string_view text) {
    return text.empty() ? std::nullopt : parse_rate_e9(text);
}

std::optional<SymbolText> perp_local(const WireInstrument& wire) {
    std::array<char, 32> buffer{};
    const auto size = wire.base_currency.size() + 1 + wire.quote_currency.size();
    if (wire.base_currency.empty() || wire.quote_currency.empty() || size > buffer.size()) {
        return std::nullopt;
    }
    auto* at = buffer.data();
    at += wire.base_currency.copy(at, wire.base_currency.size());
    *at++ = '/';
    wire.quote_currency.copy(at, wire.quote_currency.size());
    const auto local = SymbolText::from(std::string_view{buffer.data(), size});
    return local.has_value() ? std::optional{*local} : std::nullopt;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 ? fallback : UnixNanos{ms * k_ns_per_ms};
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market, UnixNanos now) {
    const bool option = market == MarketType::Option && wire.instrument_type == "option";
    const bool perp = market == MarketType::Perp && wire.instrument_type == "perp";
    if ((!option && !perp) || !wire.is_active || wire.activation_s * k_ns_per_s > now.ns) {
        return std::nullopt;
    }
    const auto tick = positive(wire.tick_size);
    const auto lot = positive(wire.amount_step);
    const auto symbol = SymbolText::from(wire.instrument_name);
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Derive, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.kind = ContractKind::Linear;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.maker_fee = parse_decimal_e9(wire.maker_fee_rate).value_or(FixedE9::undef());
    base.taker_fee = parse_decimal_e9(wire.taker_fee_rate).value_or(FixedE9::undef());

    if (perp) {
        const auto local = perp_local(wire);
        if (!local.has_value()) {
            return std::nullopt;
        }
        base.local_symbol = *local;
        return PerpInstrument{.base = base, .funding_interval_s = k_funding_basis_s};
    }

    const auto strike = steps_of(wire.strike, base.tick_size);
    if (!strike.has_value() || *strike <= 0 || wire.expiry_s == 0 ||
        (wire.option_type != "C" && wire.option_type != "P")) {
        return std::nullopt;
    }
    return OptionInstrument{.base = base,
                            .strike = PriceTicks::from_raw(*strike),
                            .expiry = UnixNanos{wire.expiry_s * k_ns_per_s},
                            .put_call = wire.option_type == "P" ? PutCall::Put : PutCall::Call,
                            .premium = OptionUnit::Quote,
                            .greeks = OptionUnit::Quote};
}

std::optional<Bbo> map_bbo(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (wire.bids.empty() || wire.asks.empty()) {
        return std::nullopt;
    }
    const auto bid = steps_of(wire.bids.front().price, base.tick_size);
    const auto ask = steps_of(wire.asks.front().price, base.tick_size);
    const auto bid_qty = steps_of(wire.bids.front().qty, base.lot_size);
    const auto ask_qty = steps_of(wire.asks.front().qty, base.lot_size);
    if (!bid.has_value() || !ask.has_value() || !bid_qty.has_value() || !ask_qty.has_value() || *bid <= 0 ||
        *ask <= 0) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = base.id;
    out.bid = PriceTicks::from_raw(*bid);
    out.ask = PriceTicks::from_raw(*ask);
    out.bid_qty = QtyLots::from_raw(*bid_qty);
    out.ask_qty = QtyLots::from_raw(*ask_qty);
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    out.venue_seq = wire.publish_id;
    using Levels = std::array<BookLevel, k_book_levels>;
    const auto fill = [&](std::span<const WireLevel> src, Levels& dst) -> std::optional<std::uint32_t> {
        std::uint32_t n = 0;
        for (const auto& level : src) {
            if (n == k_book_levels) {
                break;
            }
            const auto price = steps_of(level.price, base.tick_size);
            const auto qty = steps_of(level.qty, base.lot_size);
            if (!price.has_value() || !qty.has_value()) {
                return std::nullopt;
            }
            dst[n] = {.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
            ++n;
        }
        return n;
    };
    const auto bids = fill(wire.bids, out.bids);
    const auto asks = fill(wire.asks, out.asks);
    if (!bids.has_value() || !asks.has_value() || (*bids == 0 && *asks == 0)) {
        return std::nullopt;
    }
    out.bid_count = *bids;
    out.ask_count = *asks;
    return out;
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
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (base.id.market() != MarketType::Perp || wire.funding.empty()) {
        return std::nullopt;
    }
    const auto rate = parse_rate_e9(wire.funding);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.interval_s = k_funding_basis_s;
    out.accrual = FundingAccrual::Continuous;
    if (const auto mark = steps_of(wire.mark, base.tick_size); mark.has_value()) {
        out.mark_price = PriceTicks::from_raw(*mark);
    }
    out.ts_event = ms_to_unix(wire.ts_ms, UnixNanos{});
    out.ts_init = ts_init;
    return out;
}

std::optional<OptionGreeks> map_greeks(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (base.id.market() != MarketType::Option || !wire.priced) {
        return std::nullopt;
    }
    const options::VenueGreeks venue{.delta = figure(wire.delta),
                                     .gamma = figure(wire.gamma),
                                     .vega = figure(wire.vega),
                                     .theta = figure(wire.theta),
                                     .rho = figure(wire.rho),
                                     .mark_iv = figure(wire.mark_iv),
                                     .bid_iv = figure(wire.bid_iv),
                                     .ask_iv = figure(wire.ask_iv),
                                     .underlying_price = figure(wire.forward),
                                     .mark_price = figure(wire.mark),
                                     .index_price = figure(wire.index)};
    return options::normalise(base.id, venue, k_convention, ms_to_unix(wire.ts_ms, ts_init), ts_init);
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, MarketType market, UnixNanos ts_init) {
    const auto quote = market == MarketType::Option ? wire.premium_24h : wire.notional_24h;
    if (wire.contracts_24h.empty() && quote.empty()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = parse_amount_e9(wire.contracts_24h).value_or(FixedE9::from_raw(0));
    out.volume_24h_quote = parse_turnover_e6(quote).value_or(FixedE6::from_raw(0));
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::derive
