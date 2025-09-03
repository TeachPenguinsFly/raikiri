#include <raikiri/exec/rate_budget.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace raikiri::exec {
namespace {

constexpr std::uint64_t k_second = 1'000'000'000ULL;

RateBudget::Table table_with(RateBucket bucket, RateBudget::BucketConfig cfg) {
    RateBudget::Table t{};
    t[static_cast<std::size_t>(bucket)] = cfg;
    return t;
}

// A venue nobody has measured must read as unlimited. Refusing traffic the
// venue would have accepted is the worse of the two errors, so an empty table
// limits nothing rather than limiting everything.
TEST(RateBudget, AnUnconfiguredBucketAllowsEverything) {
    RateBudget budget{RateBudget::Table{}, {}};
    for (int i = 0; i < 500; ++i) {
        EXPECT_TRUE(
            budget.take(RateBucket::Ledger, 100, MonoNanos{0}, RateClass::Poll).has_value());
    }
}

// Capacity with no window would refill infinitely fast: configured-looking and
// limiting nothing. That reads as a silent failure, so it is treated as off.
TEST(RateBudget, ACapacityWithNoWindowIsOffRatherThanInstant) {
    RateBudget budget{table_with(RateBucket::Shared, {.capacity = 5, .window_ns = 0}), {}};
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(budget.take(RateBucket::Shared, 1, MonoNanos{0}, RateClass::Poll).has_value());
    }
}

// bybit meters each endpoint separately: spending the ledger's allowance must
// not touch what positions may still ask for.
TEST(RateBudget, BucketsAreSpentIndependently) {
    RateBudget::Table t{};
    t[static_cast<std::size_t>(RateBucket::Account)] = {.capacity = 50, .window_ns = k_second};
    t[static_cast<std::size_t>(RateBucket::Ledger)] = {.capacity = 25, .window_ns = k_second};
    RateBudget budget{t, {}};
    for (int i = 0; i < 25; ++i) {
        ASSERT_TRUE(budget.take(RateBucket::Ledger, 1, MonoNanos{0}, RateClass::Poll).has_value());
    }
    EXPECT_FALSE(budget.take(RateBucket::Ledger, 1, MonoNanos{0}, RateClass::Poll).has_value());
    EXPECT_TRUE(budget.take(RateBucket::Account, 1, MonoNanos{0}, RateClass::Poll).has_value())
        << "a drained ledger must not close the account meter";
}

// The whole reason the class of a request is carried at all.
TEST(RateBudget, PollsCannotSpendTheReserveOrdersStillNeed) {
    RateBudget budget{
        table_with(RateBucket::Shared, {.capacity = 10, .window_ns = k_second, .reserve = 4}), {}};
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(budget.take(RateBucket::Shared, 1, MonoNanos{0}, RateClass::Poll).has_value());
    }
    EXPECT_FALSE(budget.take(RateBucket::Shared, 1, MonoNanos{0}, RateClass::Poll).has_value());
    EXPECT_TRUE(budget.take(RateBucket::Shared, 4, MonoNanos{0}, RateClass::Order).has_value());
}

TEST(RateBudget, AVenueRefusalDrainsOnlyTheBucketItAnswered) {
    RateBudget::Table t{};
    t[static_cast<std::size_t>(RateBucket::Account)] = {.capacity = 50, .window_ns = k_second};
    t[static_cast<std::size_t>(RateBucket::Ledger)] = {.capacity = 25, .window_ns = k_second};
    RateBudget budget{t, {}};
    budget.penalize(RateBucket::Ledger, MonoNanos{0}, k_second);
    EXPECT_FALSE(
        budget.take(RateBucket::Ledger, 1, MonoNanos{k_second / 2}, RateClass::Order).has_value());
    EXPECT_TRUE(
        budget.take(RateBucket::Account, 1, MonoNanos{k_second / 2}, RateClass::Order).has_value());
}

// The gateway's order path and its account poller run on DIFFERENT THREADS and
// share one budget, because the venue counts them together.
TEST(RateBudget, TwoThreadsCannotBetweenThemSpendMoreThanTheBudget) {
    constexpr std::uint32_t k_capacity = 2000;
    RateBudget budget{
        table_with(RateBucket::Shared, {.capacity = k_capacity, .window_ns = k_second}), {}};
    std::atomic<int> granted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                if (budget.take(RateBucket::Shared, 1, MonoNanos{0}, RateClass::Order)
                        .has_value()) {
                    granted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(granted.load(), static_cast<int>(k_capacity))
        << "the bucket must hand out exactly its capacity, whoever asks";
}

} // namespace
} // namespace raikiri::exec
