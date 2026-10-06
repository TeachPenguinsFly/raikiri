#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/vest/map.hpp>

#include <string>

namespace raikiri::venues::vest {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_interval_ns = std::uint64_t{k_funding_interval_s} * 1'000'000'000ULL;

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

std::optional<FixedE9> decimal_step(std::uint32_t decimals) noexcept {
    if (decimals > 9) {
        return std::nullopt;
    }
    std::int64_t raw = k_e9;
    for (std::uint32_t i = 0; i < decimals; ++i) {
        raw /= 10;
    }
    return FixedE9::from_raw(raw);
}

AssetClass class_of(std::string_view asset) noexcept {
    if (asset == "crypto") {
        return AssetClass::Crypto;
    }
    if (asset == "stock") {
        return AssetClass::Equity;
    }
    if (asset == "forex") {
        return AssetClass::FX;
    }
    return AssetClass::Unknown;
}

} // namespace

UnixNanos next_settlement(UnixNanos at) noexcept {
    return UnixNanos{(at.ns / k_interval_ns + 1U) * k_interval_ns};
}

std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market) {
    if (market != MarketType::Perp || wire.status != "TRADING") {
        return std::nullopt;
    }
    const auto tick = decimal_step(wire.price_decimals);
    const auto lot = decimal_step(wire.size_decimals);
    const auto cls = class_of(wire.asset);
    // a currency pair keeps both legs: nok-usd is the krone, nok the stock
    std::string ticker{wire.base.substr(0, wire.base.find('-'))};
    if (cls == AssetClass::FX) {
        ticker.clear();
        for (const char c : wire.base) {
            if (c != '-') {
                ticker.push_back(c);
            }
        }
    }
    const auto symbol = SymbolText::from(wire.symbol);
    const auto local = canonical_symbol(ticker, wire.quote);
    if (!tick.has_value() || !lot.has_value() || !symbol.has_value() || !local.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Vest, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = FixedE9::from_raw(k_e9);
    base.kind = ContractKind::Linear;
    base.asset_class = cls;
    if (cls == AssetClass::FX) {
        if (const auto key = UnderlyingText::from(ticker); key.has_value()) {
            base.underlying = *key;
        }
    }
    if (const auto fee = parse_rate_e9(wire.taker_fee); fee.has_value()) {
        base.taker_fee = *fee;
    }
    return PerpInstrument{base, k_funding_interval_s};
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = UnixNanos{wire.ts_ms * k_ns_per_ms};
    out.ts_init = ts_init;
    const auto fill = [&](std::span<const WireLevel> src,
                          std::array<BookLevel, k_book_levels>& dst) -> std::optional<std::uint32_t> {
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

std::optional<Bbo> top_of(const BookSnapshot& book) noexcept {
    if (book.bid_count == 0 || book.ask_count == 0) {
        return std::nullopt;
    }
    return Bbo{.instrument = book.instrument,
               .bid = book.bids[0].price,
               .ask = book.asks[0].price,
               .bid_qty = book.bids[0].qty,
               .ask_qty = book.asks[0].qty,
               .ts_event = book.ts_event,
               .ts_init = book.ts_init};
}

std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init) {
    Side aggressor{};
    if (wire.side == "buy") {
        aggressor = Side::Buy;
    } else if (wire.side == "sell") {
        aggressor = Side::Sell;
    } else {
        return std::nullopt;
    }
    auto qty_text = wire.qty;
    if (qty_text.starts_with('-')) {
        qty_text.remove_prefix(1);
    }
    const auto price = steps_of(wire.price, base.tick_size);
    const auto qty = steps_of(qty_text, base.lot_size);
    if (!price.has_value() || !qty.has_value() || *qty == 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = aggressor;
    out.ts_event = UnixNanos{wire.ts_ms * k_ns_per_ms};
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate> map_funding(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto rate = parse_rate_e9(wire.funding);
    if (!rate.has_value()) {
        return std::nullopt;
    }
    FundingUpdate out{};
    out.instrument = base.id;
    out.rate = *rate;
    if (const auto mark = parse_amount_e9(wire.mark); mark.has_value() && base.tick_size.raw > 0) {
        out.mark_price = PriceTicks::from_raw(mark->raw / base.tick_size.raw);
    }
    if (const auto index = parse_amount_e9(wire.index); index.has_value() && base.tick_size.raw > 0) {
        out.index_price = PriceTicks::from_raw(index->raw / base.tick_size.raw);
    }
    out.next_funding_time = next_settlement(ts_init);
    out.interval_s = k_funding_interval_s;
    out.accrual = FundingAccrual::Discrete;
    out.ts_init = ts_init;
    return out;
}

TickerStats map_ticker(const Stats& stats, const InstrumentBase& base, UnixNanos ts_init) {
    TickerStats out{};
    out.instrument = base.id;
    out.volume_24h_base = stats.volume_base;
    out.volume_24h_quote = stats.volume_quote;
    out.open_interest = stats.open_interest;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::vest
