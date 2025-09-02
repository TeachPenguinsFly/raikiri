#pragma once

// One venue's published budget, held where every client of that venue can see
// it.
//
// TWO SHAPES, NOT ONE. binance meters a single pool of WEIGHT — 2400 a minute,
// and its income ledger costs thirty of it against five for a balance — so one
// bucket describes it. bybit meters EACH ENDPOINT separately, fifty a second
// for positions and orders but twenty-five for the transaction log, so one
// bucket would refuse traffic the venue would have taken. A budget therefore
// holds several buckets and the caller names which it is spending from.
//
// SHARED, AND THEREFORE LOCKED. A gateway runs two adapters against one venue —
// the order path and the account poller — and the venue counts them together,
// because its budget is per IP rather than per connection. Two private buckets
// would each believe they had the whole allowance and together spend twice it.
// The lock costs tens of nanoseconds on a path that is already making a network
// round trip.

#include <raikiri/exec/order_gate.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <mutex>

namespace raikiri::exec {

// The meters a venue keeps. Named by what they measure rather than by endpoint,
// so a venue with one pool uses one of them and says so.
enum class RateBucket : std::uint8_t {
    Shared = 0, // a single pool covering everything (binance)
    OrderEntry, // create / cancel / amend
    Account,    // positions, balances, open orders
    Ledger,     // income, transaction log, account book — the expensive pulls
    Count_,
};

class RateBudget {
public:
    struct BucketConfig {
        std::uint32_t capacity{0}; // 0 disables this bucket
        std::uint64_t window_ns{0};
        std::uint32_t reserve{0}; // what only order traffic may spend
    };

    // What one call costs in the venue's own units, carried beside the buckets
    // so a caller names an intent rather than a number.
    struct Costs {
        std::uint32_t order{1};
        std::uint32_t positions{1};
        std::uint32_t balances{1};
        std::uint32_t ledger{1};
    };

    using Table = std::array<BucketConfig, static_cast<std::size_t>(RateBucket::Count_)>;

    RateBudget(const Table& table, Costs costs) noexcept;

    [[nodiscard]] const Costs& costs() const noexcept { return costs_; }

    // Spends from `bucket`, or refuses with how long to wait. A bucket that was
    // configured with capacity 0 always allows: a venue whose budget we have
    // not established must not have one invented for it, because refusing
    // traffic the venue would have accepted is the worse of the two errors.
    [[nodiscard]] std::expected<void, RateDenied>
    take(RateBucket bucket, std::uint32_t cost, MonoNanos now, RateClass cls) noexcept;

    // The venue refused. Drains that bucket and holds it for `backoff_ns`: its
    // answer outranks our accounting, which has just been proven wrong.
    void penalize(RateBucket bucket, MonoNanos now, std::uint64_t backoff_ns) noexcept;

    [[nodiscard]] std::uint64_t denials(RateBucket bucket) noexcept;

private:
    std::mutex mu_;
    std::array<RateLimiter, static_cast<std::size_t>(RateBucket::Count_)> buckets_;
    // An unconfigured bucket is UNLIMITED, not closed. RateLimiter reads a
    // capacity of zero as "refuse everything", which is a legitimate state for
    // it and the wrong reading of a venue nobody has measured — so the
    // distinction is kept here, where the decision was made.
    std::array<bool, static_cast<std::size_t>(RateBucket::Count_)> limited_{};
    Costs costs_{};
};

// NO NUMBERS LIVE IN THIS LIBRARY. The budgets are operator settings, read from
// config/venues.yaml, and the gateway translates them into a Table before
// constructing a budget. exec depends on domain and nothing else, which is why
// the translation happens above rather than here.

} // namespace raikiri::exec
