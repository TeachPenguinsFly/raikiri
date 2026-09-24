#include <raikiri/exec/order_gate.hpp>

#include <gtest/gtest.h>

namespace raikiri::exec {
namespace {

constexpr std::uint64_t k_minute = 60ULL * 1'000'000'000ULL;
constexpr std::uint64_t k_second = 1'000'000'000ULL;

TEST(RateLimiter, SpendsAndRefusesWithoutBlocking) {
    RateLimiter limiter{1'200, k_minute};
    for (int i = 0; i < 1'200; ++i) {
        ASSERT_TRUE(limiter.take(1, MonoNanos{0}).has_value()) << i;
    }
    const auto denied = limiter.take(1, MonoNanos{0});
    ASSERT_FALSE(denied.has_value());
    EXPECT_GT(denied.error().retry_after_ns, 0U);
    EXPECT_EQ(limiter.denials(), 1U);
}

TEST(RateLimiter, WeightedCostsDrainFaster) {
    // binance charges endpoint weight, so take() accepts a cost
    RateLimiter limiter{2'400, k_minute};
    ASSERT_TRUE(limiter.take(40, MonoNanos{0}).has_value());
    EXPECT_EQ(limiter.available(MonoNanos{0}), 2'360U);
}

TEST(RateLimiter, RefillIsContinuousNotWindowed) {
    RateLimiter limiter{100, k_minute};
    ASSERT_TRUE(limiter.take(100, MonoNanos{0}).has_value());
    EXPECT_EQ(limiter.available(MonoNanos{0}), 0U);
    EXPECT_EQ(limiter.available(MonoNanos{k_minute / 2}), 50U);
    EXPECT_EQ(limiter.available(MonoNanos{k_minute}), 100U);
}

TEST(RateLimiter, AWindowCannotBeSpentTwiceOverByRefilling) {
    RateLimiter limiter{100, 1'000'000'000ULL};
    MonoNanos now{0};
    ASSERT_TRUE(limiter.take(100, now).has_value()) << "the bucket starts full";

    now.ns += 500'000'000ULL;
    const auto refused = limiter.take(50, now);
    ASSERT_FALSE(refused.has_value());
    EXPECT_GT(refused.error().retry_after_ns, 0U) << "waiting for the window IS the answer";

    now.ns += 500'000'000ULL;
    EXPECT_TRUE(limiter.take(50, now).has_value());
}

TEST(RateLimiter, TheWindowCeilingCountsWhatWasSpentAndNotWhatWasAsked) {
    RateLimiter limiter{10, 1'000'000'000ULL};
    MonoNanos now{0};
    ASSERT_TRUE(limiter.take(10, now).has_value());
    for (int attempt = 0; attempt < 5; ++attempt) {
        now.ns += 1'000'000ULL;
        EXPECT_FALSE(limiter.take(1, now).has_value());
    }
    now.ns += 1'000'000'000ULL;
    EXPECT_TRUE(limiter.take(10, now).has_value()) << "the whole window is available again";
}

TEST(RateLimiter, RefillNeverExceedsCapacity) {
    RateLimiter limiter{100, k_minute};
    EXPECT_EQ(limiter.available(MonoNanos{10 * k_minute}), 100U);
}

TEST(RateLimiter, AVenue429OverridesOurAccounting) {
    RateLimiter limiter{100, k_minute};
    EXPECT_EQ(limiter.available(MonoNanos{0}), 100U);
    limiter.penalize(MonoNanos{0}, 5'000'000'000ULL);

    const auto denied = limiter.take(1, MonoNanos{1'000'000'000ULL});
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().retry_after_ns, 4'000'000'000ULL);
    EXPECT_TRUE(limiter.take(1, MonoNanos{6'000'000'000ULL}).has_value());
}

TEST(RateLimiter, ACostLargerThanTheBucketIsRefusedImmediately) {
    RateLimiter limiter{10, k_minute};
    const auto denied = limiter.take(11, MonoNanos{0});
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().retry_after_ns, 0U);
}

InFlight request(std::uint64_t seq, std::uint64_t sent_ns) {
    return {.client_order_id = ClientOrderId::make(1, seq),
            .instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{0}),
            .kind = CmdKind::Create,
            .sent = MonoNanos{sent_ns}};
}

TEST(RateLimiter, APollMayNotSpendTheReserveAnOrderStillNeeds) {
    RateLimiter limiter{10, k_minute, 4};
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(limiter.take(1, MonoNanos{0}, RateClass::Poll).has_value()) << "poll " << i;
    }
    EXPECT_FALSE(limiter.take(1, MonoNanos{0}, RateClass::Poll).has_value());
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(limiter.take(1, MonoNanos{0}, RateClass::Order).has_value()) << "order " << i;
    }
    EXPECT_FALSE(limiter.take(1, MonoNanos{0}, RateClass::Order).has_value());
}

TEST(RateLimiter, APollLargerThanTheBudgetOutsideTheReserveIsRefusedOutright) {
    RateLimiter limiter{10, k_minute, 4};
    const auto denied = limiter.take(7, MonoNanos{0}, RateClass::Poll);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().retry_after_ns, 0U) << "waiting cannot make room";
    EXPECT_TRUE(limiter.take(7, MonoNanos{0}, RateClass::Order).has_value());
}

TEST(RateLimiter, AvailableReportsWhatEachClassCanStillSpend) {
    RateLimiter limiter{10, k_minute, 4};
    EXPECT_EQ(limiter.available(MonoNanos{0}, RateClass::Order), 10U);
    EXPECT_EQ(limiter.available(MonoNanos{0}, RateClass::Poll), 6U);
    ASSERT_TRUE(limiter.take(6, MonoNanos{0}, RateClass::Poll).has_value());
    EXPECT_EQ(limiter.available(MonoNanos{0}, RateClass::Poll), 0U);
    EXPECT_EQ(limiter.available(MonoNanos{0}, RateClass::Order), 4U);
}

TEST(InFlightTable, ResolvingRemovesAndUnknownIdsAreNormal) {
    InFlightTable table{8};
    ASSERT_TRUE(table.add(request(1, 0)));
    ASSERT_TRUE(table.add(request(2, 0)));
    EXPECT_TRUE(table.resolve(ClientOrderId::make(1, 1)));
    EXPECT_EQ(table.size(), 1U);
    EXPECT_FALSE(table.resolve(ClientOrderId::make(1, 99)));
}

TEST(InFlightTable, SilentRequestsAgeOutSoNoOrderHangsForever) {
    InFlightTable table{8};
    ASSERT_TRUE(table.add(request(1, 0)));
    ASSERT_TRUE(table.add(request(2, 9'000'000'000ULL)));

    const auto expired = table.expire(MonoNanos{10'000'000'000ULL}, 5'000'000'000ULL);
    ASSERT_EQ(expired.size(), 1U);
    EXPECT_EQ(expired[0].client_order_id.raw, ClientOrderId::make(1, 1).raw);
    EXPECT_EQ(table.size(), 1U) << "the young request stays";
    EXPECT_EQ(table.expired_total(), 1U);
}

TEST(InFlightTable, ExpiryHandlesEveryEntryBeingStale) {
    InFlightTable table{8};
    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(table.add(request(i, 0)));
    }
    EXPECT_EQ(table.expire(MonoNanos{k_minute}, 1'000ULL).size(), 5U);
    EXPECT_EQ(table.size(), 0U);
}

TEST(InFlightTable, AFullTableRefusesRatherThanUntrack) {
    InFlightTable table{2};
    ASSERT_TRUE(table.add(request(1, 0)));
    ASSERT_TRUE(table.add(request(2, 0)));
    EXPECT_FALSE(table.add(request(3, 0)));
}

} // namespace

TEST(InFlightTable, AnUnansweredOrderSurvivesUntilItIsSweptOrResolved) {
    InFlightTable table{8};
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{7},
                           .instrument = InstrumentId{},
                           .kind = CmdKind::Create,
                           .sent = MonoNanos{0}}));
    EXPECT_EQ(table.size(), 1U);
    EXPECT_TRUE(table.expire(MonoNanos{k_second}, 15 * k_second).empty());
    EXPECT_EQ(table.size(), 1U);

    const auto swept = table.expire(MonoNanos{20 * k_second}, 15 * k_second);
    ASSERT_EQ(swept.size(), 1U);
    EXPECT_EQ(swept.front().client_order_id.raw, 7U);
    EXPECT_EQ(table.size(), 0U) << "handed back exactly once";
    EXPECT_EQ(table.expired_total(), 1U);
}

TEST(InFlightTable, AnAnsweredOrderIsNeverSweptLater) {
    InFlightTable table{8};
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{1}, .sent = MonoNanos{0}}));
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{2}, .sent = MonoNanos{0}}));
    EXPECT_TRUE(table.resolve(ClientOrderId{1}));
    const auto swept = table.expire(MonoNanos{20 * k_second}, 15 * k_second);
    ASSERT_EQ(swept.size(), 1U);
    EXPECT_EQ(swept.front().client_order_id.raw, 2U);
    EXPECT_EQ(table.expired_total(), 1U) << "the answered one was never a gap";
}

TEST(InFlightTable, ResolvingAnUnknownIdChangesNothing) {
    InFlightTable table{8};
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{1}, .sent = MonoNanos{0}}));
    EXPECT_FALSE(table.resolve(ClientOrderId{99}));
    EXPECT_EQ(table.size(), 1U);
}

TEST(InFlightTable, TheEntryCarriesTheIdTheVenueKnowsItBy) {
    InFlightTable table{4};
    const auto link = *ExchangeClientOrderId::from("01M1EENQW5DXEXVXKC3726V503");
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{3},
                           .exchange_client_order_id = link,
                           .side = Side::Sell,
                           .qty = QtyLots::from_raw(52),
                           .kind = CmdKind::Cancel,
                           .sent = MonoNanos{0}}));
    const auto swept = table.expire(MonoNanos{20 * k_second}, k_second);
    ASSERT_EQ(swept.size(), 1U);
    EXPECT_EQ(swept.front().exchange_client_order_id.view(), link.view());
    EXPECT_EQ(swept.front().side, Side::Sell);
    EXPECT_EQ(swept.front().qty.raw, 52);
    EXPECT_EQ(swept.front().kind, CmdKind::Cancel);
}

TEST(InFlightTable, AFullTableRefusesRatherThanForgetting) {
    InFlightTable table{2};
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{1}, .sent = MonoNanos{0}}));
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{2}, .sent = MonoNanos{0}}));
    EXPECT_FALSE(table.add({.client_order_id = ClientOrderId{3}, .sent = MonoNanos{0}}));
    EXPECT_EQ(table.size(), 2U);
}

} // namespace raikiri::exec
