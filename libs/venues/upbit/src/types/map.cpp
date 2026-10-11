#include <raikiri/common/int128.hpp>
#include <raikiri/venues/upbit/map.hpp>

#include <array>
#include <limits>
#include <span>

namespace raikiri::venues::upbit {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_ns_per_us = 1'000ULL;

// a bound on what one level may be worth, in units of the quote
[[nodiscard]] std::int64_t largest_level(std::string_view quote) noexcept {
    if (quote == "KRW") {
        return 10'000'000'000'000;
    }
    if (quote == "BTC") {
        return 10'000;
    }
    return 10'000'000'000;
}

std::optional<std::int64_t> price_of(std::string_view text, const InstrumentBase& base) {
    const auto ticks = parse_steps(text, base.tick_size);
    return ticks.has_value() && *ticks > 0 ? ticks : std::nullopt;
}

std::optional<std::int64_t> qty_of(std::string_view text, const InstrumentBase& base) {
    return parse_steps(text, base.lot_size, false);
}

// a level of zero price pads a short side; a zero size is skipped
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
        const auto qty = qty_of(level.qty, base);
        if (!price.has_value() || !qty.has_value() || *price < 0) {
            return std::nullopt;
        }
        if (*price == 0) {
            break;
        }
        if (*qty == 0) {
            continue;
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

FixedE9 lot_for(std::string_view quote, std::string_view price) noexcept {
    constexpr std::int64_t k_finest = 10;
    const auto at = parse_number_e9(price);
    if (!at.has_value() || at->raw <= 0) {
        return FixedE9::from_raw(k_finest);
    }
    constexpr auto k_e18 = static_cast<int128_t>(1'000'000'000'000'000'000LL);
    const int128_t most = static_cast<int128_t>(largest_level(quote)) * k_e18 / at->raw;
    constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
    std::int64_t lot = k_finest;
    while (most / lot > k_i64_max && lot < k_e9) {
        lot *= 10;
    }
    return FixedE9::from_raw(lot);
}

std::optional<Instrument> map_instrument(const WireMarket& wire, std::string_view price, VenueId venue, UnixNanos now) {
    const auto dash = wire.market.find('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= wire.market.size()) {
        return std::nullopt;
    }
    const auto quote = wire.market.substr(0, dash);
    const auto base_ccy = wire.market.substr(dash + 1);
    std::array<char, 32> buffer{};
    if (base_ccy.size() + 1 + quote.size() > buffer.size()) {
        return std::nullopt;
    }
    auto* at = buffer.data();
    at += base_ccy.copy(at, base_ccy.size());
    *at++ = '/';
    at += quote.copy(at, quote.size());
    const auto local = SymbolText::from(std::string_view{buffer.data(), static_cast<std::size_t>(at - buffer.data())});
    const auto symbol = SymbolText::from(wire.market);
    if (!local.has_value() || !symbol.has_value()) {
        return std::nullopt;
    }
    InstrumentBase base{};
    // provisional: InstrumentTable::add assigns the SymbolId
    base.id = InstrumentId::make(venue, MarketType::Spot, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *local;
    base.kind = ContractKind::Linear;
    base.tick_size = k_price_step;
    base.lot_size = lot_for(quote, price);
    base.contract_value = FixedE9::from_raw(k_e9);
    base.ts_loaded = now;
    return SpotInstrument{.base = base};
}

UnixNanos stamp(std::uint64_t ts) noexcept {
    if (ts == 0) {
        return UnixNanos{};
    }
    constexpr std::uint64_t k_ms_until = 100'000'000'000'000ULL;
    return UnixNanos{ts < k_ms_until ? ts * k_ns_per_ms : ts * k_ns_per_us};
}

std::optional<Bbo> map_bbo(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto book = map_book(wire, base, ts_init);
    if (!book.has_value() || book->bid_count == 0 || book->ask_count == 0) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = base.id;
    out.bid = book->bids[0].price;
    out.ask = book->asks[0].price;
    out.bid_qty = book->bids[0].qty;
    out.ask_qty = book->asks[0].qty;
    out.ts_event = book->ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init) {
    BookSnapshot out{};
    out.instrument = base.id;
    out.ts_event = stamp(wire.ts);
    out.ts_init = ts_init;
    out.venue_seq = wire.ts;
    const auto bids = fill_side(wire.bids, base, true, out.bids);
    const auto asks = fill_side(wire.asks, base, false, out.asks);
    if (!bids.has_value() || !asks.has_value()) {
        return std::nullopt;
    }
    if (*bids > 0 && *asks > 0 && out.bids[0].price.raw >= out.asks[0].price.raw) {
        return std::nullopt;
    }
    out.bid_count = *bids;
    out.ask_count = *asks;
    return out;
}

std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init) {
    const auto price = price_of(wire.price, base);
    const auto qty = qty_of(wire.qty, base);
    if (!price.has_value() || !qty.has_value() || *qty <= 0) {
        return std::nullopt;
    }
    TradeTick out{};
    out.instrument = base.id;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.aggressor = wire.aggressor_bought ? Side::Buy : Side::Sell;
    out.ts_event = stamp(wire.ts_ms);
    out.ts_init = ts_init;
    return out;
}

std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, bool stamped, UnixNanos ts_init) {
    if (wire.volume_24h.empty() && wire.turnover_24h.empty()) {
        return std::nullopt;
    }
    TickerStats out{};
    out.instrument = base.id;
    // a won-quoted meme coin trades more units a day than e9 holds
    out.volume_24h_base = parse_amount_e9(wire.volume_24h).value_or(FixedE9::undef());
    out.volume_24h_quote = parse_turnover_e6(wire.turnover_24h).value_or(FixedE6::from_raw(0));
    out.open_interest = FixedE9::undef();
    out.ts_event = stamped ? stamp(wire.ts_ms) : UnixNanos{};
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::upbit
