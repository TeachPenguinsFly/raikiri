#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/ostium/map.hpp>

#include <charconv>
#include <string>

namespace raikiri::venues::ostium {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;
constexpr std::int64_t k_e9_raw = 1'000'000'000;
constexpr std::uint32_t k_whole_percent = 10'000;

std::optional<std::int64_t> parse_i64(std::string_view text) noexcept {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

AssetClass class_of(std::string_view group) noexcept {
    if (group == "crypto") {
        return AssetClass::Crypto;
    }
    if (group == "stocks" || group == "etf") {
        return AssetClass::Equity;
    }
    if (group == "indices") {
        return AssetClass::Index;
    }
    if (group == "commodities") {
        return AssetClass::Commodity;
    }
    if (group == "forex") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

bool closes(std::string_view action) noexcept {
    return action == "Close" || action == "StopLoss" || action == "TakeProfit" || action == "Liquidation" ||
           action == "CloseDayTrade";
}

std::optional<std::int64_t> per_hour(std::int64_t per_block) noexcept {
    return per_block * k_blocks_per_hour / k_e9_raw;
}

} // namespace

std::optional<Instrument> map_instrument(const WirePair& wire, MarketType market) {
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    const auto cut = wire.symbol.find('-');
    const auto ticker = wire.symbol.substr(0, cut);
    // settled in usdc; a pair quoted in another currency keeps it
    const auto quote = wire.to == "USD" ? std::string_view{"USDC"} : wire.to;
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(ticker, quote);
    if (cut == std::string_view::npos || ticker.empty() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Ostium, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = FixedE9::from_raw(k_step_raw);
    base.lot_size = FixedE9::from_raw(k_step_raw);
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = class_of(wire.group);
    if (base.asset_class == AssetClass::FX) {
        std::string pair{wire.from};
        pair += wire.to;
        if (const auto key = UnderlyingText::from(pair); key.has_value()) {
            base.underlying = *key;
        }
    }
    return PerpInstrument{base, k_rate_unit_s};
}

std::optional<FixedE9> e18_to_e9(std::string_view text) noexcept {
    const bool negative = !text.empty() && text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos) {
        return std::nullopt;
    }
    if (text.size() <= 9) {
        return FixedE9::from_raw(0);
    }
    const auto whole = parse_i64(text.substr(0, text.size() - 9));
    if (!whole.has_value()) {
        return std::nullopt;
    }
    return FixedE9::from_raw(negative ? -*whole : *whole);
}

std::optional<PriceTicks> mid_ticks(const WireQuote& wire) noexcept {
    const auto mid = parse_amount_e9(wire.mid);
    if (!mid.has_value() || mid->raw <= 0) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(mid->raw / k_step_raw);
}

std::optional<Bbo> map_bbo(const WireQuote& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto bid = parse_amount_e9(wire.bid);
    const auto ask = parse_amount_e9(wire.ask);
    if (!bid.has_value() || !ask.has_value() || bid->raw <= 0 || ask->raw < bid->raw || base.tick_size.raw <= 0) {
        return std::nullopt;
    }
    return Bbo{.instrument = base.id,
               .bid = PriceTicks::from_raw(bid->raw / base.tick_size.raw),
               .ask = PriceTicks::from_raw(ask->raw / base.tick_size.raw),
               .bid_qty = QtyLots::from_raw(0),
               .ask_qty = QtyLots::from_raw(0),
               .ts_event = UnixNanos{wire.ts_ms * k_ns_per_ms},
               .ts_init = ts_init};
}

std::optional<FixedE9> long_rate_per_hour(const WireRollover& wire) noexcept {
    const auto pure = parse_i64(wire.long_pure);
    const auto premium = parse_i64(wire.premium);
    if (!pure.has_value() || !premium.has_value()) {
        return std::nullopt;
    }
    auto per_block = *pure + *premium;
    if (!wire.negative_allowed && per_block < 0) {
        per_block = 0;
    }
    const auto rate = per_hour(per_block);
    return rate.has_value() ? std::optional<FixedE9>{FixedE9::from_raw(*rate)} : std::nullopt;
}

std::optional<FundingUpdate>
map_funding(const WireRollover& wire, const InstrumentBase& base, PriceTicks mid, UnixNanos ts_init) {
    const auto rate = long_rate_per_hour(wire);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    out.mark_price = mid;
    out.index_price = mid;
    out.interval_s = k_rate_unit_s;
    out.accrual = FundingAccrual::Continuous;
    out.ts_init = ts_init;
    return out;
}

std::optional<FillPrint> map_fill(const WireFill& wire, const InstrumentBase& base, UnixNanos ts_init) {
    Side aggressor{};
    if (wire.action == "Open") {
        aggressor = wire.is_buy ? Side::Buy : Side::Sell;
    } else if (closes(wire.action)) {
        aggressor = wire.is_buy ? Side::Sell : Side::Buy;
    } else {
        return std::nullopt;
    }
    const auto price = e18_to_e9(wire.price);
    auto size = e18_to_e9(wire.size);
    if (!price.has_value() || !size.has_value() || price->raw <= 0 || size->raw <= 0 || base.tick_size.raw <= 0 ||
        base.lot_size.raw <= 0) {
        return std::nullopt;
    }
    if (closes(wire.action) && wire.close_percent > 0 && wire.close_percent < k_whole_percent) {
        size = FixedE9::from_raw(
            static_cast<std::int64_t>(static_cast<int128_t>(size->raw) * wire.close_percent / k_whole_percent));
    }
    FillPrint out{};
    out.trade.instrument = base.id;
    out.trade.price = PriceTicks::from_raw(price->raw / base.tick_size.raw);
    out.trade.qty = QtyLots::from_raw(size->raw / base.lot_size.raw);
    out.trade.aggressor = aggressor;
    out.trade.ts_event = UnixNanos{wire.executed_s * k_ns_per_s};
    out.trade.ts_init = ts_init;
    out.quote = FixedE6::from_raw(
        static_cast<std::int64_t>(static_cast<int128_t>(price->raw) * size->raw / 1'000'000'000'000LL));
    if (out.trade.qty.raw <= 0) {
        return std::nullopt;
    }
    return out;
}

TickerStats map_ticker(const WireRollover& wire,
                       const InstrumentBase& base,
                       FixedE9 volume_24h_base,
                       FixedE6 volume_24h_quote,
                       UnixNanos ts_init) {
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = volume_24h_base;
    out.volume_24h_quote = volume_24h_quote;
    const auto long_oi = e18_to_e9(wire.long_oi);
    const auto short_oi = e18_to_e9(wire.short_oi);
    if (long_oi.has_value() && short_oi.has_value()) {
        out.open_interest = FixedE9::from_raw(long_oi->raw + short_oi->raw);
    }
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::ostium
