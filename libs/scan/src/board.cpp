#include <raikiri/scan/board.hpp>

namespace raikiri::scan {

namespace {

constexpr std::int64_t k_seconds_per_hour = 3600;
constexpr std::int64_t k_hours_per_settlement = 8;
constexpr std::int64_t k_bp_scale = 10'000;

// A quote is usable only if both sides are present, positive and the right way
// round. UNDEF IS NOT ZERO here either: a leg whose venue has said nothing has
// no price, which is a different fact from a price of nothing.
[[nodiscard]] bool quoted(const Leg& leg) noexcept {
    return !leg.bid.is_undef() && !leg.ask.is_undef() && leg.bid.raw > 0 && leg.ask.raw > 0;
}

} // namespace

std::optional<FixedE9> per_hour(FixedE9 rate, std::uint32_t interval_s) noexcept {
    if (interval_s == 0 || rate.is_undef()) {
        return std::nullopt;
    }
    // rate * 3600 / interval. The multiply comes first so a sub-hourly venue
    // does not truncate to zero on the way: hyperliquid's hourly rate divided
    // first would lose every digit it has.
    return FixedE9::from_raw(
        mul_div(rate.raw, k_seconds_per_hour, static_cast<std::int64_t>(interval_s)));
}

std::expected<Row, Reject> pair(const Leg& a, const Leg& b, FixedE9 min_volume_quote) noexcept {
    if (a.venue == b.venue) {
        return std::unexpected{Reject::OneVenue};
    }
    const auto hourly_a = per_hour(a.rate, a.interval_s);
    const auto hourly_b = per_hour(b.rate, b.interval_s);
    if (!hourly_a.has_value() || !hourly_b.has_value()) {
        return std::unexpected{Reject::NoInterval};
    }
    if (!quoted(a) || !quoted(b)) {
        return std::unexpected{Reject::NoQuote};
    }
    if (a.bid.raw > a.ask.raw || b.bid.raw > b.ask.raw) {
        return std::unexpected{Reject::CrossedBook};
    }
    if (!min_volume_quote.is_undef() && min_volume_quote.raw > 0) {
        const auto thin = [&](const Leg& leg) {
            return leg.volume_24h_quote.is_undef() ||
                   leg.volume_24h_quote.raw < min_volume_quote.raw;
        };
        if (thin(a) || thin(b)) {
            return std::unexpected{Reject::ThinVolume};
        }
    }

    // THE RATES DECIDE THE ORIENTATION, not the caller and not the price. A
    // positive funding rate is longs paying shorts, so the leg to be short of
    // is whichever charges more per hour, and the leg to hold is the other.
    // Ties orient by the pair's own order, which is stable and means nothing.
    const bool short_is_a = hourly_a->raw > hourly_b->raw;
    const Leg& short_leg = short_is_a ? a : b;
    const Leg& long_leg = short_is_a ? b : a;
    const auto short_hourly = short_is_a ? *hourly_a : *hourly_b;
    const auto long_hourly = short_is_a ? *hourly_b : *hourly_a;

    Row row{.long_leg = long_leg, .short_leg = short_leg};
    row.funding_edge_8h =
        FixedE9::from_raw((short_hourly.raw - long_hourly.raw) * k_hours_per_settlement);

    // Sell the short leg at ITS bid, buy the long leg at ITS ask — the two
    // prices a taker actually gets. Against the mid of those same two, so the
    // number is symmetric in the pair.
    const auto sum = short_leg.bid.raw + long_leg.ask.raw;
    row.price_spread_bp = mul_div(short_leg.bid.raw - long_leg.ask.raw, 2 * k_bp_scale, sum);

    row.volume_floor =
        FixedE9::from_raw(std::min(a.volume_24h_quote.is_undef() ? 0 : a.volume_24h_quote.raw,
                                   b.volume_24h_quote.is_undef() ? 0 : b.volume_24h_quote.raw));
    return row;
}

} // namespace raikiri::scan
