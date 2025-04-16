#include <raikiri/common/fixed_point.hpp>
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

} // namespace

std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int32_t exponent = 0;
    if (const auto marker = text.find_first_of("eE"); marker != std::string_view::npos) {
        // Trimmed by offset rather than substr, which throws and this is
        // noexcept.
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
    // Inexact means more precision than FixedE9 models — PEPEUSDTM's 1.0E-10
    // tick is one decade past it. Widening the domain scale is the only real
    // fix, and that is not a per-venue decision.
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
    // tickerV2's `ts` is already nanoseconds; multiplying it as milliseconds
    // would put every quote 31 million years into the future.
    out.ts_event = ns_to_unix(wire.ts_ns, ts_init);
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

std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                         std::string_view symbol,
                                         const exec::InstrumentTable& table,
                                         UnixNanos ts_init) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto rate = to_e9(wire.rate);
    if (!rate.has_value()) {
        return std::nullopt; // contract quotes no funding: not the same as zero
    }

    FundingUpdate out{};
    out.instrument = base_of(*instrument).id;
    out.rate = *rate;
    if (const auto predicted = to_e9(wire.predicted_rate); predicted.has_value()) {
        out.predicted_rate = *predicted;
    }
    // mark_price stays undefined: KuCoin publishes mark under its own subject,
    // and a zero here would read as a real price of zero.
    //
    // The interval comes from the catalogue, never from the frame's own
    // `granularity`: the stream republishes once a minute and says 60000 there,
    // which would report an 8 h contract as funding every minute.
    const auto* perp = std::get_if<PerpInstrument>(instrument);
    const std::uint64_t interval_ms =
        perp == nullptr ? 0 : static_cast<std::uint64_t>(perp->funding_interval_s) * 1000ULL;
    out.interval_s = perp == nullptr ? 0 : perp->funding_interval_s;
    // Only the REST form states the next settlement. Settlements land on
    // wall-clock multiples of the interval — deriving the next boundary that
    // way reproduces the REST answer exactly (verified), and leaving it at
    // zero would read as "funding never".
    out.next_funding_time =
        wire.next_funding_ms != 0 ? ms_to_unix(wire.next_funding_ms, UnixNanos{0})
        : (interval_ms != 0 && wire.ts_ms != 0)
            ? ms_to_unix((wire.ts_ms / interval_ms + 1) * interval_ms, UnixNanos{0})
            : UnixNanos{0};
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market) {
    // api-futures.kucoin.com serves derivatives only; spot lives behind a
    // different host and a different catalogue shape entirely.
    if (market != MarketType::Perp) {
        return std::nullopt;
    }
    if (wire.status != "Open") {
        return std::nullopt;
    }
    // FFWCSX is the perpetual; FFICSX is a dated future sharing this endpoint,
    // and asking for perps and getting one back is a whole category of
    // position bug.
    if (wire.type != "FFWCSX") {
        return std::nullopt;
    }

    const auto tick = parse_number_e9(wire.tick_size);
    const auto lot = parse_number_e9(wire.lot_size);
    // One CONTRACT is `multiplier` of the base currency (0.001 XBT on
    // XBTUSDTM), so every size on the wire needs it to become a notional; a
    // hardcoded 1 would misprice XBTUSDTM by 1000x. It is negative on inverse
    // contracts, where size is quoted in USD instead — that is a different
    // instrument model, so those rows are refused here rather than stored with
    // a negative size that later arithmetic would silently honour.
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
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *multiplier;
    // min_notional stays undefined: KuCoin's floor is a contract count
    // (maxOrderQty bounds the other end), and converting it to a notional
    // needs a price we do not have here. A zero would read as "no minimum".
    //
    // Unlike okx, the catalogue does carry the funding interval, and KuCoin
    // runs 1h, 4h and 8h contracts side by side — so this is read rather than
    // guessed.
    return PerpInstrument{base, static_cast<std::uint32_t>(wire.funding_granularity_ms / 1000)};
}

} // namespace raikiri::venues::kucoin
