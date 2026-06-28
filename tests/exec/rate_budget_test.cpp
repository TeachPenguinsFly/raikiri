#include <raikiri/common/sliding_log.hpp>
#include <raikiri/exec/rate_budget.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <random>
#include <thread>
#include <vector>

namespace raikiri::exec {
namespace {

constexpr std::uint64_t k_second = 1'000'000'000ULL;

RateLimit pool(std::uint32_t max, std::uint64_t window_ns, std::uint32_t reserve = 0) {
    return {.name = "pool", .window_ns = window_ns, .max = max, .reserve = reserve, .match = {}};
}

TEST(RateBudget, NoLimitsAllowsEverything) {
    RateBudget budget{{}, {}};
    for (int i = 0; i < 500; ++i) {
        EXPECT_TRUE(budget.take("/anything", MonoNanos{0}, RateClass::Poll).has_value());
    }
}

// Every window of length W, wherever it starts, holds at most `max` — so a
// burst at the end of one fixed window and another at the start of the next
// cannot together exceed it.
TEST(SlidingLog, NoWindowAnywhereHoldsMoreThanMax) {
    constexpr std::uint32_t k_max = 10;
    constexpr std::uint64_t k_window = 1000;
    SlidingLog log{k_max, k_window};
    std::vector<std::uint64_t> sent;
    std::mt19937_64 rng{7};
    std::uint64_t now = 0;
    for (int i = 0; i < 20'000; ++i) {
        now += rng() % 60;
        if (log.wait_ns(1, now) == 0) {
            log.spend(1, now);
            sent.push_back(now);
        }
    }
    ASSERT_GT(sent.size(), 100U);
    for (std::size_t i = 0; i < sent.size(); ++i) {
        std::size_t inside = 0;
        for (std::size_t j = i; j < sent.size() && sent[j] < sent[i] + k_window; ++j) {
            ++inside;
        }
        ASSERT_LE(inside, k_max) << "window starting at " << sent[i];
    }
}

TEST(SlidingLog, AFullLogAcrossAFixedWindowBoundaryWaitsForTheOldestToAge) {
    SlidingLog log{10, 1000};
    for (int i = 0; i < 10; ++i) {
        log.spend(1, 990); // the end of fixed window [0, 1000)
    }
    EXPECT_EQ(log.wait_ns(1, 1000), 990U) << "the next fixed window is not a fresh allowance";
    EXPECT_EQ(log.wait_ns(1, 1990), 0U);
}

TEST(RateBudget, ARequestSpendsFromEveryLimitThatMatchesIt) {
    RateBudget budget{{pool(100, k_second),
                       {.name = "orders", .window_ns = k_second, .max = 2, .match = {"/trade/"}}},
                      {}};
    EXPECT_TRUE(budget.take("/trade/order", MonoNanos{0}, RateClass::Order).has_value());
    EXPECT_TRUE(budget.take("/trade/order", MonoNanos{0}, RateClass::Order).has_value());
    EXPECT_FALSE(budget.take("/trade/order", MonoNanos{0}, RateClass::Order).has_value());
    EXPECT_TRUE(budget.take("/market/books", MonoNanos{0}, RateClass::Poll).has_value())
        << "a path the order limit does not match spends only the pool";
    EXPECT_EQ(budget.denials(), 1U);
}

TEST(RateBudget, ARefusedRequestSpendsFromNoLimitAtAll) {
    RateBudget budget{{pool(3, k_second),
                       {.name = "narrow", .window_ns = k_second, .max = 1, .match = {"/a"}}},
                      {}};
    ASSERT_TRUE(budget.take("/a", MonoNanos{0}, RateClass::Poll).has_value());
    ASSERT_FALSE(budget.take("/a", MonoNanos{0}, RateClass::Poll).has_value());
    EXPECT_TRUE(budget.take("/b", MonoNanos{0}, RateClass::Poll).has_value());
    EXPECT_TRUE(budget.take("/b", MonoNanos{0}, RateClass::Poll).has_value())
        << "the refused /a must not have spent the pool";
}

TEST(RateBudget, PathsMatchByPrefixAndQueryPairAndTheLongestCostWins) {
    EXPECT_TRUE(path_matches("/fapi/v1/depth", "/fapi/v1/depth?symbol=X&limit=100"));
    EXPECT_TRUE(path_matches("/fapi/v1/depth?limit=100", "/fapi/v1/depth?symbol=X&limit=100"));
    EXPECT_FALSE(path_matches("/fapi/v1/depth?limit=100", "/fapi/v1/depth?symbol=X&limit=1000"));
    EXPECT_FALSE(path_matches("/fapi/v1/depth?limit=100", "/fapi/v1/depth"));
    EXPECT_FALSE(path_matches("/api/v5/trade/", "/api/v5/account/bills"));

    RateBudget budget{{pool(40, k_second)},
                      {{.match = "/depth", .cost = 20}, {.match = "/depth?limit=100", .cost = 5}},
                      3};
    EXPECT_EQ(budget.cost("/depth?limit=100"), 5U);
    EXPECT_EQ(budget.cost("/depth?limit=7"), 20U);
    EXPECT_EQ(budget.cost("/other"), 3U);
    EXPECT_TRUE(budget.take("/depth", MonoNanos{0}, RateClass::Poll).has_value());
    EXPECT_TRUE(budget.take("/depth", MonoNanos{0}, RateClass::Poll).has_value());
    EXPECT_FALSE(budget.take("/depth?limit=100", MonoNanos{0}, RateClass::Poll).has_value())
        << "40 spent by two weight-20 calls";
}

TEST(RateBudget, AFreePathSpendsNothing) {
    RateBudget budget{{pool(1, k_second)}, {{.match = "/public", .cost = 0}}};
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(budget.take("/public/instruments", MonoNanos{0}, RateClass::Poll).has_value());
    }
    EXPECT_TRUE(budget.take("/private", MonoNanos{0}, RateClass::Poll).has_value());
}

TEST(RateBudget, APollCannotSpendTheReserveAnOrderNeeds) {
    RateBudget budget{{pool(10, k_second, 4)}, {}};
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(budget.take("/x", MonoNanos{0}, RateClass::Poll).has_value());
    }
    EXPECT_FALSE(budget.take("/x", MonoNanos{0}, RateClass::Poll).has_value());
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(budget.take("/x", MonoNanos{0}, RateClass::Order).has_value());
    }
    EXPECT_FALSE(budget.take("/x", MonoNanos{0}, RateClass::Order).has_value());
}

TEST(RateBudget, APenaltyHoldsOnlyTheLimitsItsPathSpends) {
    RateBudget budget{{{.name = "a", .window_ns = k_second, .max = 50, .match = {"/a"}},
                       {.name = "b", .window_ns = k_second, .max = 50, .match = {"/b"}}},
                      {}};
    budget.penalize("/a/x", MonoNanos{0}, k_second);
    const auto held = budget.take("/a/x", MonoNanos{k_second / 2}, RateClass::Order);
    ASSERT_FALSE(held.has_value());
    EXPECT_EQ(held.error().retry_after_ns, k_second / 2);
    EXPECT_TRUE(budget.take("/b/x", MonoNanos{k_second / 2}, RateClass::Order).has_value());
    EXPECT_TRUE(budget.take("/a/x", MonoNanos{k_second}, RateClass::Order).has_value());
    budget.penalize_all(MonoNanos{k_second}, k_second);
    EXPECT_FALSE(budget.take("/b/x", MonoNanos{k_second}, RateClass::Order).has_value());
}

TEST(RateBudget, TwoThreadsCannotBetweenThemSpendMoreThanTheBudget) {
    constexpr std::uint32_t k_capacity = 2000;
    RateBudget budget{{pool(k_capacity, k_second)}, {}};
    std::atomic<int> granted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                if (budget.take("/x", MonoNanos{0}, RateClass::Order).has_value()) {
                    granted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(granted.load(), static_cast<int>(k_capacity));
}

} // namespace
} // namespace raikiri::exec
