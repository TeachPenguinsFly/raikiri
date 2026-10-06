#include <raikiri/common/int128.hpp>
#include <raikiri/venues/bitstamp/map.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <span>

namespace raikiri::venues::bitstamp {

namespace {

constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;
constexpr std::uint64_t k_ns_per_us = 1'000ULL;
constexpr std::uint32_t k_max_decimals = 9;
constexpr auto k_fiat = std::to_array<std::string_view>({"USD", "EUR", "GBP", "CHF"});
constexpr auto k_prefixes = std::to_array<std::string_view>({"order_book_", "live_trades_", "funding_rate_"});

[[nodiscard]] bool fiat(std::string_view currency) noexcept {
    return std::ranges::find(k_fiat, currency) != k_fiat.end();
}

[[nodiscard]] std::optional<FixedE9> step_of(std::uint32_t decimals) noexcept {
    if (decimals > k_max_decimals) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::uint32_t i = 0; i < decimals; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

[[nodiscard]] AssetClass stated_class(std::string_view cls) noexcept {
    if (cls == "CRYPTO") {
        return AssetClass::Crypto;
    }
    if (cls == "COMMODITIES") {
        return AssetClass::Commodity;
    }
    if (cls == "ETF") {
        return AssetClass::Equity;
    }
    if (cls == "FX") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

std::optional<SymbolText> joined(std::string_view left, std::string_view sep, std::string_view right) {
    std::array<char, 32> buffer{};
    const auto size = left.size() + sep.size() + right.size();
    if (left.empty() || right.empty() || size > buffer.size()) {
        return std::nullopt;
    }
    auto* at = buffer.data();
    at += left.copy(at, left.size());
    at += sep.copy(at, sep.size());
    right.copy(at, right.size());
    const auto out = SymbolText::from(std::string_view{buffer.data(), size});
    return out.has_value() ? std::optional{*out} : std::nullopt;
}

std::optional<std::uint32_t> fill_side(std::span<const WireLevel> src,
                                       const InstrumentBase& base,
                                       bool bids,
                                       std::array<BookLevel, k_book_levels>& dst) {
    std::uint32_t n = 0;
    for (const auto& level : src) {
        if (n == dst.size()) {
            break;
        }
        const auto price = parse_steps(level.price, base.tick_size);
        const auto qty = parse_steps(level.qty, base.lot_size);
        if (!price.has_value() || !qty.has_value() || *price <= 0 || *qty <= 0) {
            return std::nullopt;
        }
        if (n > 0 && (bids ? *price >= dst[n - 1].price.raw : *price <= dst[n - 1].price.raw)) {
            return std::nullopt;
        }
        dst[n] = {.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
        ++n;
    }
    return n;
}

} // namespace

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market, UnixNanos now) {
    const bool perp = wire.market_type == "PERPETUAL";
    if (wire.trading != "Enabled" || (perp ? market != MarketType::Perp : market != MarketType::Spot) ||
        (!perp && wire.market_type != "SPOT")) {
        return std::nullopt;
    }
    const auto tick = step_of(wire.counter_decimals);
    const auto lot = step_of(wire.base_decimals);
    const auto symbol = SymbolText::from(wire.market_symbol);
    const auto local = joined(wire.base_currency, "/", wire.counter_currency);
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(VenueId::Bitstamp, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.kind = ContractKind::Linear;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.min_notional = parse_decimal_e9(wire.minimum_order_value).value_or(FixedE9::undef());
    base.contract_value = FixedE9::from_raw(k_e9);
    base.ts_loaded = now;
    const bool fx = fiat(wire.base_currency) && fiat(wire.counter_currency);
    base.asset_class = perp ? stated_class(wire.asset_class) : fx ? AssetClass::FX : AssetClass::Crypto;
    if (base.asset_class == AssetClass::FX) {
        if (const auto pair = joined(wire.base_currency, "", wire.counter_currency); pair.has_value()) {
            if (const auto key = UnderlyingText::from(pair->view()); key.has_value()) {
                base.underlying = *key;
            }
        }
    }
    if (!perp) {
        return SpotInstrument{.base = base};
    }
    const auto size = parse_decimal_e9(wire.contract_size);
    if (!size.has_value() || size->raw <= 0) {
        return std::nullopt;
    }
    base.contract_value = *size;
    return PerpInstrument{.base = base, .funding_interval_s = k_funding_interval_s};
}

std::string_view symbol_of(std::string_view channel) noexcept {
    for (const auto prefix : k_prefixes) {
        if (channel.starts_with(prefix)) {
            return channel.substr(prefix.size());
        }
    }
    return {};
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = UnixNanos{wire.ts_us * k_ns_per_us};
    out.ts_init = ts_init;
    out.venue_seq = wire.ts_us;
    const auto bids = fill_side(wire.bids, base, true, out.bids);
    const auto asks = fill_side(wire.asks, base, false, out.asks);
    if (!bids.has_value() || !asks.has_value() || *bids == 0 || *asks == 0 ||
        out.bids[0].price.raw >= out.asks[0].price.raw) {
        return std::nullopt;
    }
    out.bid_count = *bids;
    out.ask_count = *asks;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = parse_steps(wire.price, base.tick_size);
    const auto qty = parse_steps(wire.qty, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *price <= 0 || *qty <= 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = UnixNanos{wire.ts_us * k_ns_per_us};
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireFunding& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (base.id.market() != MarketType::Perp) {
        return std::nullopt;
    }
    const auto rate = parse_rate_e9(wire.rate);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    if (const auto mark = parse_steps(wire.mark, base.tick_size, false); mark.has_value()) {
        out.mark_price = PriceTicks::from_raw(*mark);
    }
    if (const auto index = parse_steps(wire.index, base.tick_size, false); index.has_value()) {
        out.index_price = PriceTicks::from_raw(*index);
    }
    out.next_funding_time = UnixNanos{wire.next_s * k_ns_per_s};
    out.ts_event = UnixNanos{wire.ts_s * k_ns_per_s};
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats> map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    if (wire.volume.empty()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = parse_amount_e9(wire.volume).value_or(FixedE9::undef());
    out.volume_24h_quote = FixedE6::from_raw(0);
    const auto lots = parse_steps(wire.volume, base.lot_size, false);
    const auto vwap = parse_number_e9(wire.vwap);
    if (lots.has_value() && vwap.has_value()) {
        constexpr int128_t k_e12 = 1'000'000'000'000;
        constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
        const int128_t volume = static_cast<int128_t>(*lots) * base.lot_size.raw;
        const int128_t quote = vwap->raw > 0 && volume < k_i64_max * k_i64_max / vwap->raw ? volume * vwap->raw / k_e12
                                                                                           : -1;
        if (quote >= 0 && quote <= k_i64_max) {
            out.volume_24h_quote = FixedE6::from_raw(static_cast<std::int64_t>(quote));
        }
    }
    if (const auto open = parse_amount_e9(wire.open_interest); open.has_value()) {
        out.open_interest = *open;
    }
    out.ts_event = UnixNanos{wire.ts_s * k_ns_per_s};
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::bitstamp
