#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>
#include <raikiri/venues/deribit/map.hpp>

#include <limits>
#include <optional>

namespace raikiri::venues::deribit {

namespace {

// The rate's UNIT, not a payment cadence — see map_funding.
constexpr std::uint32_t k_funding_basis_s = 8 * 60 * 60;

// UnixNanos wraps past ~2554-07-21 in the millisecond multiply, and Deribit
// parks perpetuals at 32503708800000 (year 3000) as an expiry sentinel.
constexpr std::uint64_t k_max_expiry_ms = std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL;

// Real Deribit exponents live in [-5, 5]; this only bounds the shift below.
constexpr int k_max_exponent = 60;

[[nodiscard]] constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr std::optional<uint128_t> pow10(int n) noexcept {
    if (n < 0 || n > 38) { // 10^38 is the last that fits 128 bits
        return std::nullopt;
    }
    uint128_t out = 1;
    for (int i = 0; i < n; ++i) {
        out *= 10;
    }
    return out;
}

constexpr uint128_t k_mantissa_cap = *pow10(30);

} // namespace

// The value is mantissa * 10^(exponent - fraction_digits), so its e9 form is
// that mantissa shifted by 9 + exponent - fraction_digits: a multiply when the
// shift is positive, an EXACT division when negative. Failing to divide evenly
// means more precision than FixedE9 models, which is refused, not rounded.
std::optional<FixedE9> parse_number_e9(std::string_view token) noexcept {
    if (token.empty()) {
        return std::nullopt;
    }
    std::size_t i = 0;
    bool negative = false;
    if (token[i] == '+' || token[i] == '-') {
        negative = token[i] == '-';
        ++i;
    }

    uint128_t mantissa = 0;
    int fraction_digits = 0;
    bool seen_point = false;
    bool seen_digit = false;
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (c == '.') {
            if (seen_point) {
                return std::nullopt;
            }
            seen_point = true;
            continue;
        }
        if (c == 'e' || c == 'E') {
            break;
        }
        if (!is_digit(c)) {
            return std::nullopt;
        }
        seen_digit = true;
        mantissa = (mantissa * 10) + static_cast<unsigned>(c - '0');
        if (mantissa > k_mantissa_cap) {
            return std::nullopt;
        }
        if (seen_point) {
            ++fraction_digits;
        }
    }
    if (!seen_digit) {
        return std::nullopt;
    }

    int exponent = 0;
    if (i < token.size()) {
        ++i; // the 'e'
        bool exponent_negative = false;
        if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
            exponent_negative = token[i] == '-';
            ++i;
        }
        bool seen_exponent_digit = false;
        for (; i < token.size(); ++i) {
            if (!is_digit(token[i])) {
                return std::nullopt;
            }
            seen_exponent_digit = true;
            exponent = (exponent * 10) + (token[i] - '0');
            if (exponent > k_max_exponent) {
                return std::nullopt;
            }
        }
        if (!seen_exponent_digit) {
            return std::nullopt;
        }
        if (exponent_negative) {
            exponent = -exponent;
        }
    }

    const int shift = 9 + exponent - fraction_digits;
    uint128_t scaled = 0;
    if (shift >= 0) {
        const auto factor = pow10(shift);
        if (!factor.has_value()) {
            return std::nullopt;
        }
        scaled = mantissa * *factor;
    } else {
        const auto divisor = pow10(-shift);
        if (!divisor.has_value() || mantissa % *divisor != 0) {
            return std::nullopt;
        }
        scaled = mantissa / *divisor;
    }
    if (scaled > static_cast<uint128_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    const auto raw = static_cast<std::int64_t>(scaled);
    return FixedE9::from_raw(negative ? -raw : raw);
}

namespace {

// Exact conversion against the instrument's own step. Failing to divide evenly
// means Deribit changed an instrument's precision, so the value is dropped:
// rounding is how a reference price lands a tick off what actually printed.
std::optional<std::int64_t> exact_steps(std::string_view token, FixedE9 step) {
    if (token.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(token);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

std::optional<PriceTicks> to_ticks(std::string_view token, const InstrumentBase& base) {
    const auto steps = exact_steps(token, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> to_lots(std::string_view token, const InstrumentBase& base) {
    const auto steps = exact_steps(token, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

} // namespace

UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept {
    return ms == 0 || ms > k_max_expiry_ms ? fallback : UnixNanos{ms * 1'000'000ULL};
}

std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
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
    if (bid->raw <= 0 || ask->raw <= 0) {
        return std::nullopt;
    }

    Bbo out{};
    out.instrument = base.id;
    out.bid = *bid;
    out.ask = *ask;
    out.bid_qty = *bid_qty;
    out.ask_qty = *ask_qty;
    // The one timestamp is the matching-engine one; there is no send time.
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
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
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.instrument);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    if (wire.current_funding.empty()) {
        return std::nullopt;
    }
    const auto rate = parse_number_e9(wire.current_funding);
    if (!rate.has_value()) {
        return std::nullopt;
    }

    FundingUpdate out{};
    const auto& base = base_of(*instrument);
    out.instrument = base.id;
    out.rate = *rate;
    out.interval_s = k_funding_basis_s;
    if (const auto mark = to_ticks(wire.mark_price, base); mark.has_value()) {
        out.mark_price = *mark;
    }
    out.ts_event = ms_to_unix(wire.ts_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market) {
    if (!wire.is_active) {
        return std::nullopt; // delivered, expired or not yet listed
    }
    const bool perpetual = wire.settlement_period == "perpetual";
    switch (market) {
    case MarketType::Perp:
        if (wire.kind != "future" || !perpetual) {
            return std::nullopt;
        }
        break;
    case MarketType::Future:
        // Both share kind "future", and confusing them is a position bug.
        if (wire.kind != "future" || perpetual) {
            return std::nullopt;
        }
        break;
    case MarketType::Option:
        if (wire.kind != "option") {
            return std::nullopt;
        }
        break;
    case MarketType::Spot:
        return std::nullopt; // listed, but the caps row calls it vestigial
    }
    if (wire.instrument_type != "reversed") {
        return std::nullopt;
    }

    const auto tick = parse_number_e9(wire.tick_size);
    const auto lot = parse_number_e9(wire.min_trade_amount);
    const auto contract = parse_number_e9(wire.contract_size);
    if (!tick.has_value() || !lot.has_value() || !contract.has_value() || tick->raw <= 0 ||
        lot->raw <= 0 || contract->raw <= 0) {
        return std::nullopt;
    }
    const auto symbol = SymbolText::from(wire.instrument_name);
    if (!symbol.has_value()) {
        return std::nullopt;
    }

    InstrumentBase base{};
    // Provisional: InstrumentTable::add assigns the SymbolId that indexes it.
    base.id = InstrumentId::make(VenueId::Deribit, market, SymbolId{0});
    base.exchange_symbol = *symbol;
    base.local_symbol = *symbol;
    base.tick_size = *tick;
    base.lot_size = *lot;
    base.contract_value = *contract;
    // Deribit states the direction itself and the guard above already refused
    // anything but "reversed", so every row reaching here is the venue's own
    // word for inverse: contract_size is USD, settlement is in the base coin.
    base.kind = ContractKind::Inverse;

    if (market == MarketType::Option) {
        const auto strike = exact_steps(wire.strike, base.tick_size);
        if (!strike.has_value() || *strike <= 0) {
            return std::nullopt;
        }
        if (wire.expiration_ms == 0 || wire.expiration_ms > k_max_expiry_ms) {
            return std::nullopt;
        }
        if (wire.option_type != "put" && wire.option_type != "call") {
            return std::nullopt;
        }
        const auto put_call = wire.option_type == "put" ? PutCall::Put : PutCall::Call;
        return OptionInstrument{base,
                                PriceTicks::from_raw(*strike),
                                ms_to_unix(wire.expiration_ms, UnixNanos{0}),
                                put_call};
    }

    base.min_notional = *lot;

    if (market == MarketType::Future) {
        if (wire.expiration_ms == 0 || wire.expiration_ms > k_max_expiry_ms) {
            return std::nullopt;
        }
        return FutureInstrument{base, ms_to_unix(wire.expiration_ms, UnixNanos{0})};
    }
    return PerpInstrument{base, /*funding_interval_s=*/0};
}

} // namespace raikiri::venues::deribit
