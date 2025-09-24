#include <raikiri/exec/order_gate.hpp>

#include <gtest/gtest.h>

namespace raikiri::exec {
namespace {

constexpr std::uint64_t k_minute = 60ULL * 1'000'000'000ULL;
constexpr std::uint64_t k_second = 1'000'000'000ULL;

TEST(RateLimiter, SpendsAndRefusesWithoutBlocking) {
    // Binance's order budget: 1200 per minute. The limiter refuses rather
    // than sleeps — blocking in submit() would stall the same loop that
    // drains cancels, so a rate-limited new order would also delay the
    // escape hatch.
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
    // Binance charges endpoint WEIGHT, not requests: one account-wide
    // openOrders pull costs as much as forty cheap reads, which is exactly
    // why the limiter takes a cost instead of counting calls.
    RateLimiter limiter{2'400, k_minute};
    ASSERT_TRUE(limiter.take(40, MonoNanos{0}).has_value());
    EXPECT_EQ(limiter.available(MonoNanos{0}), 2'360U);
}

TEST(RateLimiter, RefillIsContinuousNotWindowed) {
    // A window that resets on the minute means our clock and the venue's
    // disagree about where the boundary is, and a burst spanning the reset is
    // how an account earns a ban. Half a window returns half the capacity.
    RateLimiter limiter{100, k_minute};
    ASSERT_TRUE(limiter.take(100, MonoNanos{0}).has_value());
    EXPECT_EQ(limiter.available(MonoNanos{0}), 0U);
    EXPECT_EQ(limiter.available(MonoNanos{k_minute / 2}), 50U);
    EXPECT_EQ(limiter.available(MonoNanos{k_minute}), 100U);
}

TEST(RateLimiter, AWindowCannotBeSpentTwiceOverByRefilling) {
    // THE DEFECT THIS EXISTS FOR. The bucket starts full and refills
    // continuously, so spending it all and then spending the refill puts twice
    // the venue's allowance through one window — measured against binance's
    // 2400 a minute, roughly 4560, with nothing refused. A full 571-instrument
    // seed round is 2855 weight and fits in that gap.
    RateLimiter limiter{100, 1'000'000'000ULL};
    MonoNanos now{0};
    ASSERT_TRUE(limiter.take(100, now).has_value()) << "the bucket starts full";

    // Half a window later the bucket has earned fifty back, and the bucket
    // alone would hand them over. The window has not turned, so it must not.
    now.ns += 500'000'000ULL;
    const auto refused = limiter.take(50, now);
    ASSERT_FALSE(refused.has_value());
    EXPECT_GT(refused.error().retry_after_ns, 0U) << "waiting for the window IS the answer";

    // Past the window it opens again, and only then.
    now.ns += 500'000'000ULL;
    EXPECT_TRUE(limiter.take(50, now).has_value());
}

TEST(RateLimiter, TheWindowCeilingCountsWhatWasSpentAndNotWhatWasAsked) {
    // A refusal must not consume the allowance it was refused: a caller that
    // retries every tick would otherwise hold its own budget shut.
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
    // The venue knows better than our model. A 429 drains the bucket and
    // blocks for the backoff it asked for, even though we believed we had
    // budget left.
    RateLimiter limiter{100, k_minute};
    EXPECT_EQ(limiter.available(MonoNanos{0}), 100U);
    limiter.penalize(MonoNanos{0}, 5'000'000'000ULL);

    const auto denied = limiter.take(1, MonoNanos{1'000'000'000ULL});
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().retry_after_ns, 4'000'000'000ULL);
    EXPECT_TRUE(limiter.take(1, MonoNanos{6'000'000'000ULL}).has_value());
}

TEST(RateLimiter, ACostLargerThanTheBucketIsRefusedImmediately) {
    // Waiting would never help; this is a misconfigured cost, not congestion,
    // so retry_after is zero rather than infinite.
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

// THE REASON THE CLASS OF A REQUEST EXISTS. A cancel that cannot be sent
// because a reconciliation sweep spent the budget is an open position the
// strategy believes is closed.
TEST(RateLimiter, APollMayNotSpendTheReserveAnOrderStillNeeds) {
    RateLimiter limiter{10, k_minute, 4};
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(limiter.take(1, MonoNanos{0}, RateClass::Poll).has_value()) << "poll " << i;
    }
    EXPECT_FALSE(limiter.take(1, MonoNanos{0}, RateClass::Poll).has_value());
    // The reserve is untouched, so the order path still goes through.
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(limiter.take(1, MonoNanos{0}, RateClass::Order).has_value()) << "order " << i;
    }
    EXPECT_FALSE(limiter.take(1, MonoNanos{0}, RateClass::Order).has_value());
}

// A poll asking for more than the budget minus the reserve can never succeed,
// so it is told to stop rather than handed a retry that will fail again.
TEST(RateLimiter, APollLargerThanTheBudgetOutsideTheReserveIsRefusedOutright) {
    RateLimiter limiter{10, k_minute, 4};
    const auto denied = limiter.take(7, MonoNanos{0}, RateClass::Poll);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().retry_after_ns, 0U) << "waiting cannot make room";
    // The same cost is affordable to an order, which may spend the reserve.
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
    // Venues also report on orders placed before this process started; that
    // is not an error, it is what reconciliation is for.
    EXPECT_FALSE(table.resolve(ClientOrderId::make(1, 99)));
}

TEST(InFlightTable, SilentRequestsAgeOutSoNoOrderHangsForever) {
    // The failure this class exists for: REST can time out AFTER the venue
    // accepted the order, so no event will ever arrive. Ageing it out lets
    // the adapter synthesize a terminal event and hand the real answer to
    // reconciliation.
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
    // Swap-and-pop while iterating is easy to get wrong when the whole table
    // qualifies; this pins that none are skipped.
    InFlightTable table{8};
    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(table.add(request(i, 0)));
    }
    EXPECT_EQ(table.expire(MonoNanos{k_minute}, 1'000ULL).size(), 5U);
    EXPECT_EQ(table.size(), 0U);
}

TEST(InFlightTable, AFullTableRefusesRatherThanUntrack) {
    // An untracked order can never be aged out, so a full table means venue
    // down — never "send it anyway".
    InFlightTable table{2};
    ASSERT_TRUE(table.add(request(1, 0)));
    ASSERT_TRUE(table.add(request(2, 0)));
    EXPECT_FALSE(table.add(request(3, 0)));
}

} // namespace

// The failure this table exists for: a REST call that fails AFTER the exchange
// took the order. The entry must survive the transport's silence, because the
// silence is the only evidence there is.
TEST(InFlightTable, AnUnansweredOrderSurvivesUntilItIsSweptOrResolved) {
    InFlightTable table{8};
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{7},
                           .instrument = InstrumentId{},
                           .kind = CmdKind::Create,
                           .sent = MonoNanos{0}}));
    EXPECT_EQ(table.size(), 1U);
    // Not yet stale: nothing is handed back and nothing is lost.
    EXPECT_TRUE(table.expire(MonoNanos{k_second}, 15 * k_second).empty());
    EXPECT_EQ(table.size(), 1U);

    const auto swept = table.expire(MonoNanos{20 * k_second}, 15 * k_second);
    ASSERT_EQ(swept.size(), 1U);
    EXPECT_EQ(swept.front().client_order_id.raw, 7U);
    EXPECT_EQ(table.size(), 0U) << "handed back exactly once";
    EXPECT_EQ(table.expired_total(), 1U);
}

// Any answer settles the question, including a refusal: what is tracked is
// silence, not failure.
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

// Resolving something that was never in flight must not remove a neighbour:
// the swap-and-pop below is easy to get wrong.
TEST(InFlightTable, ResolvingAnUnknownIdChangesNothing) {
    InFlightTable table{8};
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{1}, .sent = MonoNanos{0}}));
    EXPECT_FALSE(table.resolve(ClientOrderId{99}));
    EXPECT_EQ(table.size(), 1U);
}

// The venue's id travels with the entry, because a follow-up question has to
// quote the id the VENUE knows — ours never left this process.
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

// A full table must refuse rather than drop, because the entry it would drop
// is the record that an order is unaccounted for.
TEST(InFlightTable, AFullTableRefusesRatherThanForgetting) {
    InFlightTable table{2};
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{1}, .sent = MonoNanos{0}}));
    ASSERT_TRUE(table.add({.client_order_id = ClientOrderId{2}, .sent = MonoNanos{0}}));
    EXPECT_FALSE(table.add({.client_order_id = ClientOrderId{3}, .sent = MonoNanos{0}}));
    EXPECT_EQ(table.size(), 2U);
}

} // namespace raikiri::exec
