#pragma once

// which part of an arriving client order can cross inside the desk, and at what price; no clock, no state, no
// allocation, so one stream of inputs always plans the same crossings

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::internalcross {

enum class Refusal : std::uint8_t {
    NoQuote,
    StaleQuote,
    CrossedQuote,
    WideQuote,
    NoTickInside,
    OutsideLimit,
    FeeOutsideTouch,
    FeeUnpriced,
    Risk,
    NoCounterparty,
    FokShort,
    PostOnly,
    NoRoom,
};

inline constexpr std::size_t k_refusal_count = 13;

[[nodiscard]] std::string_view to_string(Refusal why) noexcept;

struct QuotePolicy {
    std::uint64_t max_quote_age_ns{1'000'000'000};
    std::int64_t max_spread_bp{20};
};

// one venue's best bid and ask for this listing, in price e9 so venues with different ticks compare
struct Touch {
    FixedE9 bid{};
    FixedE9 ask{};
    UnixNanos ts{};
    // the listing being crossed; without a live quote of its own nothing crosses
    bool own{false};
};

struct Reference {
    FixedE9 bid{};
    FixedE9 ask{};
};

// the best bid and ask over the live venues; a stale other venue is left out, a stale own book refuses
[[nodiscard]] std::expected<Reference, Refusal>
best_touch(std::span<const Touch> touches, const QuotePolicy& policy, UnixNanos now) noexcept;

enum class Rounding : std::uint8_t {
    Nearest, // between clients: half a tick goes to neither side for long, so ties go to the even tick
    Down,    // the client buys from the desk
    Up,      // the client sells to the desk
};

// the mid on the listing's tick grid; nullopt when no tick lies inside the touch
[[nodiscard]] std::optional<PriceTicks> mid_on_grid(const Reference& ref, FixedE9 tick, Rounding rounding) noexcept;

// a buy pays price plus fee, a sell gets price less fee; neither may be worse than the touch itself
[[nodiscard]] bool fee_inside_touch(const Reference& ref, FixedE9 price, Side side, std::int64_t fee_bp) noexcept;

// what crossing spares both sides against taking the venue: half the spread and the taker fee on each leg; an estimate
[[nodiscard]] FixedE9 venue_cost_saved(const Reference& ref, FixedE9 notional, FixedE9 taker_fee) noexcept;

// an order's internal fee, truncated
[[nodiscard]] FixedE9 fee_of(FixedE9 notional, std::int64_t fee_bp) noexcept;

struct Interest {
    ClientOrderId order{};
    std::uint16_t book{0};
    Side side{Side::Buy};
    // undef is a market order
    PriceTicks limit{};
    QtyLots leaves{QtyLots::from_raw(0)};
    std::int64_t fee_bp{0};
    // spot only: lots this book holds to sell, since the venue would not lend the desk more; undef is no cap
    QtyLots sell_cap{QtyLots::undef()};
};

// the hedge book as a counterparty: it only ever gets smaller
struct Inventory {
    std::uint16_t book{0};
    QtyLots position{QtyLots::from_raw(0)};
    QtyLots working_buy{QtyLots::from_raw(0)};
    QtyLots working_sell{QtyLots::from_raw(0)};
};

// lots the desk can hand a client on `client_side` that shrink it, past what its own working orders already will
[[nodiscard]] QtyLots reducible(const Inventory& desk, Side client_side) noexcept;

struct Allocation {
    // null for the desk
    ClientOrderId counter{};
    std::uint16_t book{0};
    std::int64_t counter_fee_bp{0};
    QtyLots qty{QtyLots::from_raw(0)};
    PriceTicks price{};
    bool desk{false};
};

struct Plan {
    std::size_t count{0};
    QtyLots crossed{QtyLots::from_raw(0)};
    // why nothing crossed, or why the last candidate did not
    std::optional<Refusal> why{};
};

namespace internal {

[[nodiscard]] constexpr bool limit_allows(PriceTicks limit, Side side, PriceTicks price) noexcept {
    return limit.is_undef() || (side == Side::Buy ? price.raw <= limit.raw : price.raw >= limit.raw);
}

} // namespace internal

// `opposite` is the other clients' interest, oldest first; `allowed(book, order, side, qty, price)` is the
// counterparty's risk gate. nothing is booked here, and a fill-or-kill that cannot cross whole crosses nothing
template <typename Allowed>
[[nodiscard]] Plan plan_cross(const Interest& arriving,
                              TimeInForce tif,
                              std::span<const Interest> opposite,
                              const std::optional<Inventory>& desk,
                              const Reference& ref,
                              FixedE9 tick,
                              Allowed&& allowed,
                              std::span<Allocation> out) noexcept {
    Plan plan{};
    auto left = arriving.leaves.raw;
    const auto counter_side = arriving.side == Side::Buy ? Side::Sell : Side::Buy;
    auto arriving_cap = arriving.sell_cap.is_undef() || arriving.side != Side::Sell
                            ? std::numeric_limits<std::int64_t>::max()
                            : arriving.sell_cap.raw;
    const auto price_fits = [&](PriceTicks price, const std::int64_t counter_fee, PriceTicks counter_limit) {
        if (!internal::limit_allows(arriving.limit, arriving.side, price) ||
            !internal::limit_allows(counter_limit, counter_side, price)) {
            plan.why = Refusal::OutsideLimit;
            return false;
        }
        const auto value = tick.scaled_by(price.raw);
        if (!fee_inside_touch(ref, value, arriving.side, arriving.fee_bp) ||
            !fee_inside_touch(ref, value, counter_side, counter_fee)) {
            plan.why = Refusal::FeeOutsideTouch;
            return false;
        }
        return true;
    };

    for (const auto& other : opposite) {
        if (left <= 0 || arriving_cap <= 0 || plan.count == out.size()) {
            break;
        }
        // self-cross within one book is never a trade
        if (other.book == arriving.book || other.side != counter_side || other.leaves.raw <= 0) {
            continue;
        }
        auto qty = std::min(left, other.leaves.raw);
        if (arriving.side == Side::Sell) {
            qty = std::min(qty, arriving_cap);
        } else if (!other.sell_cap.is_undef()) {
            qty = std::min(qty, other.sell_cap.raw);
        }
        if (qty <= 0) {
            continue;
        }
        const auto price = mid_on_grid(ref, tick, Rounding::Nearest);
        if (!price.has_value()) {
            plan.why = Refusal::NoTickInside;
            continue;
        }
        if (!price_fits(*price, other.fee_bp, other.limit)) {
            continue;
        }
        if (!allowed(other.book, other.order, counter_side, QtyLots::from_raw(qty), *price)) {
            plan.why = Refusal::Risk;
            continue;
        }
        out[plan.count++] = {.counter = other.order,
                             .book = other.book,
                             .counter_fee_bp = other.fee_bp,
                             .qty = QtyLots::from_raw(qty),
                             .price = *price};
        left -= qty;
        if (arriving.side == Side::Sell) {
            arriving_cap -= qty;
        }
    }

    if (left > 0 && arriving_cap > 0 && plan.count < out.size() && desk.has_value() && desk->book != arriving.book) {
        const auto room = reducible(*desk, arriving.side).raw;
        const auto qty = std::min({left, room, arriving_cap});
        if (qty > 0) {
            const auto price = mid_on_grid(ref, tick, arriving.side == Side::Buy ? Rounding::Down : Rounding::Up);
            if (!price.has_value()) {
                plan.why = Refusal::NoTickInside;
            } else if (price_fits(*price, 0, PriceTicks::undef())) {
                if (allowed(desk->book, ClientOrderId{}, counter_side, QtyLots::from_raw(qty), *price)) {
                    out[plan.count++] = {
                        .book = desk->book, .qty = QtyLots::from_raw(qty), .price = *price, .desk = true};
                    left -= qty;
                } else {
                    plan.why = Refusal::Risk;
                }
            }
        }
    }

    plan.crossed = QtyLots::from_raw(arriving.leaves.raw - left);
    if (tif == TimeInForce::Fok && left > 0) {
        plan.count = 0;
        plan.crossed = QtyLots::from_raw(0);
        plan.why = Refusal::FokShort;
    } else if (plan.count == 0 && !plan.why.has_value()) {
        plan.why = Refusal::NoCounterparty;
    }
    return plan;
}

} // namespace raikiri::internalcross
