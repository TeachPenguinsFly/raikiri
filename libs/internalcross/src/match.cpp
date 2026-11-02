#include <raikiri/internalcross/domain/match.hpp>

namespace raikiri::internalcross {

std::string_view to_string(Refusal why) noexcept {
    switch (why) {
    case Refusal::NoQuote:
        return "no_quote";
    case Refusal::StaleQuote:
        return "stale_quote";
    case Refusal::CrossedQuote:
        return "crossed_quote";
    case Refusal::WideQuote:
        return "wide_quote";
    case Refusal::NoTickInside:
        return "no_tick_inside";
    case Refusal::OutsideLimit:
        return "outside_limit";
    case Refusal::FeeOutsideTouch:
        return "fee_outside_touch";
    case Refusal::FeeUnpriced:
        return "fee_unpriced";
    case Refusal::Risk:
        return "risk";
    case Refusal::NoCounterparty:
        return "no_counterparty";
    case Refusal::FokShort:
        return "fok_short";
    case Refusal::PostOnly:
        return "post_only";
    case Refusal::NoRoom:
        return "no_room";
    }
    return "unknown";
}

std::expected<Reference, Refusal>
best_touch(std::span<const Touch> touches, const QuotePolicy& policy, UnixNanos now) noexcept {
    bool own_seen = false;
    Reference out{};
    bool any = false;
    for (const auto& touch : touches) {
        const bool quoted = !touch.bid.is_undef() && !touch.ask.is_undef() && touch.bid.raw > 0 && touch.ask.raw > 0;
        const auto age = now.ns > touch.ts.ns ? now.ns - touch.ts.ns : 0;
        const bool live = touch.ts.ns != 0 && age <= policy.max_quote_age_ns;
        if (touch.own) {
            if (!quoted) {
                return std::unexpected{Refusal::NoQuote};
            }
            if (!live) {
                return std::unexpected{Refusal::StaleQuote};
            }
            own_seen = true;
        }
        if (!quoted || !live) {
            continue;
        }
        out.bid = !any || touch.bid.raw > out.bid.raw ? touch.bid : out.bid;
        out.ask = !any || touch.ask.raw < out.ask.raw ? touch.ask : out.ask;
        any = true;
    }
    if (!own_seen) {
        return std::unexpected{Refusal::NoQuote};
    }
    if (out.bid.raw >= out.ask.raw) {
        return std::unexpected{Refusal::CrossedQuote};
    }
    // spread / mid in bp, without dividing: spread * 2 * 10000 against max * (bid + ask)
    const auto spread = static_cast<int128_t>(out.ask.raw - out.bid.raw) * 20'000;
    const auto allowed = static_cast<int128_t>(policy.max_spread_bp) *
                         (static_cast<int128_t>(out.bid.raw) + out.ask.raw);
    if (spread > allowed) {
        return std::unexpected{Refusal::WideQuote};
    }
    return out;
}

std::optional<PriceTicks> mid_on_grid(const Reference& ref, FixedE9 tick, Rounding rounding) noexcept {
    if (tick.is_undef() || tick.raw <= 0) {
        return std::nullopt;
    }
    const auto sum = static_cast<int128_t>(ref.bid.raw) + ref.ask.raw;
    const auto cell = static_cast<int128_t>(tick.raw) * 2;
    auto ticks = sum / cell;
    const auto rest = sum % cell;
    if (rest != 0) {
        const bool up = rounding == Rounding::Up ||
                        (rounding == Rounding::Nearest && (rest > tick.raw || (rest == tick.raw && (ticks % 2) != 0)));
        ticks += up ? 1 : 0;
    }
    const auto value = ticks * tick.raw;
    if (value < ref.bid.raw || value > ref.ask.raw) {
        return std::nullopt;
    }
    return PriceTicks::from_raw(static_cast<std::int64_t>(ticks));
}

bool fee_inside_touch(const Reference& ref, FixedE9 price, Side side, std::int64_t fee_bp) noexcept {
    constexpr std::int64_t k_whole = 10'000;
    if (side == Side::Buy) {
        return static_cast<int128_t>(price.raw) * (k_whole + fee_bp) <= static_cast<int128_t>(ref.ask.raw) * k_whole;
    }
    return static_cast<int128_t>(price.raw) * (k_whole - fee_bp) >= static_cast<int128_t>(ref.bid.raw) * k_whole;
}

FixedE9 venue_cost_saved(const Reference& ref, FixedE9 notional, FixedE9 taker_fee) noexcept {
    if (notional.is_undef() || notional.raw <= 0) {
        return FixedE9::from_raw(0);
    }
    const auto sum = static_cast<int128_t>(ref.bid.raw) + ref.ask.raw;
    // half the spread over the mid is spread / sum
    const auto spread_part = sum > 0 ? static_cast<int128_t>(notional.raw) * (ref.ask.raw - ref.bid.raw) / sum : 0;
    const auto fee_part = taker_fee.is_undef() || taker_fee.raw <= 0
                              ? int128_t{0}
                              : static_cast<int128_t>(notional.raw) * taker_fee.raw / k_e9;
    return FixedE9::from_raw(static_cast<std::int64_t>((spread_part + fee_part) * 2));
}

FixedE9 fee_of(FixedE9 notional, std::int64_t fee_bp) noexcept {
    if (notional.is_undef() || fee_bp <= 0) {
        return FixedE9::from_raw(0);
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(static_cast<int128_t>(notional.raw) * fee_bp / 10'000));
}

QtyLots reducible(const Inventory& desk, Side client_side) noexcept {
    const auto held = desk.position.raw;
    const auto room = client_side == Side::Buy ? held - desk.working_sell.raw : -held - desk.working_buy.raw;
    const bool shrinks = client_side == Side::Buy ? held > 0 : held < 0;
    return QtyLots::from_raw(shrinks && room > 0 ? room : 0);
}

} // namespace raikiri::internalcross
