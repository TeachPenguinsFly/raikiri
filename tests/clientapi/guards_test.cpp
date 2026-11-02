#include <raikiri/clientapi/domain/guards.hpp>
#include <raikiri/clientapi/domain/key.hpp>

#include <gtest/gtest.h>

namespace raikiri::clientapi {
namespace {

constexpr std::uint64_t k_second = 1'000'000'000ULL;

TEST(Canonical, FieldsCannotSlideIntoEachOther) {
    EXPECT_NE(canonical_request("1", "GET", "/a", "b=1", ""), canonical_request("1", "GET", "/ab=1", "", ""));
    EXPECT_EQ(canonical_request("1", "POST", "/a", "b=1", "{}"), "1\nPOST\n/a\nb=1\n{}");
}

TEST(Millis, TakesDigitsOnly) {
    EXPECT_EQ(parse_millis("1700"), 1700U);
    EXPECT_FALSE(parse_millis("").has_value());
    EXPECT_FALSE(parse_millis("17x").has_value());
    EXPECT_FALSE(parse_millis("-5").has_value());
}

TEST(Bucket, BurstThenRefusedThenRefills) {
    TokenBucket bucket{2, 3, 0};
    EXPECT_TRUE(bucket.take(0).allowed);
    EXPECT_TRUE(bucket.take(0).allowed);
    const auto last = bucket.take(0);
    EXPECT_TRUE(last.allowed);
    EXPECT_EQ(last.remaining, 0U);
    EXPECT_EQ(last.limit, 3U);
    const auto refused = bucket.take(0);
    EXPECT_FALSE(refused.allowed);
    EXPECT_EQ(refused.retry_after_s, 1U);
    EXPECT_EQ(refused.reset_s, 2U);
    EXPECT_FALSE(bucket.take(k_second / 4).allowed);
    EXPECT_TRUE(bucket.take(k_second / 2).allowed);
}

TEST(Bucket, ALongIdleFillsToCapacityAndNoFurther) {
    TokenBucket bucket{1000, 5, 0};
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bucket.take(0).allowed);
    }
    const auto far = std::uint64_t{50} * 365 * 86'400 * k_second;
    EXPECT_EQ(bucket.take(far).remaining, 4U);
}

TEST(Replay, SameSignatureOnceInsideTheWindow) {
    ReplayGuard guard{10'000};
    EXPECT_TRUE(guard.admit("sig", 1'000));
    EXPECT_FALSE(guard.admit("sig", 2'000));
    EXPECT_TRUE(guard.admit("other", 2'000));
    EXPECT_TRUE(guard.admit("sig", 11'000));
}

TEST(Replay, ForgetsWhatCanNoLongerPassTheSkewCheck) {
    ReplayGuard guard{100};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(guard.admit("sig" + std::to_string(i), static_cast<std::uint64_t>(i) * 100));
    }
    EXPECT_LE(guard.size(), 2U);
}

TEST(Idempotency, ReplaysTheFirstResult) {
    IdempotencyTable table{100};
    EXPECT_EQ(table.begin("k", "h", 0).verdict, IdemVerdict::Fresh);
    EXPECT_EQ(table.begin("k", "h", 1).verdict, IdemVerdict::InFlight);
    table.finish("k", {.status = 202, .body = "b"});
    const auto again = table.begin("k", "h", 2);
    EXPECT_EQ(again.verdict, IdemVerdict::Replay);
    EXPECT_EQ(again.reply.status, 202);
    EXPECT_EQ(again.reply.body, "b");
}

TEST(Idempotency, RefusesAnotherRequestUnderTheSameKey) {
    IdempotencyTable table{100};
    (void)table.begin("k", "h", 0);
    EXPECT_EQ(table.begin("k", "other", 1).verdict, IdemVerdict::Mismatch);
}

TEST(Idempotency, AfterAnUnansweredCallItNeverSendsAgain) {
    IdempotencyTable table{100};
    (void)table.begin("k", "h", 0);
    table.mark_unknown("k");
    EXPECT_EQ(table.begin("k", "h", 1).verdict, IdemVerdict::OutcomeUnknown);
}

TEST(Idempotency, APendingEntryFromBeforeARestartComesBackUnknown) {
    IdempotencyTable table{100};
    table.restore("k", {.request_hash = "h", .state = IdemState::Pending, .at_ns = 50}, 60);
    EXPECT_EQ(table.begin("k", "h", 61).verdict, IdemVerdict::OutcomeUnknown);
}

TEST(Idempotency, ReleasedAndExpiredKeysAreFreshAgain) {
    IdempotencyTable table{100};
    (void)table.begin("a", "h", 0);
    table.release("a");
    EXPECT_EQ(table.begin("a", "h", 1).verdict, IdemVerdict::Fresh);
    table.finish("a", {.status = 200});
    EXPECT_EQ(table.begin("a", "h", 200).verdict, IdemVerdict::Fresh);
}

TEST(IpRange, MatchesV4AndV6) {
    const auto v4 = parse_ip_range("10.1.2.0/24");
    ASSERT_TRUE(v4.has_value());
    EXPECT_TRUE(in_range(*v4, "10.1.2.200"));
    EXPECT_FALSE(in_range(*v4, "10.1.3.1"));
    EXPECT_TRUE(in_range(*v4, "::ffff:10.1.2.9"));
    const auto v6 = parse_ip_range("2001:db8::/32");
    ASSERT_TRUE(v6.has_value());
    EXPECT_TRUE(in_range(*v6, "2001:db8:1::1"));
    EXPECT_FALSE(in_range(*v6, "2001:db9::1"));
    const auto one = parse_ip_range("203.0.113.7");
    ASSERT_TRUE(one.has_value());
    EXPECT_TRUE(in_range(*one, "203.0.113.7"));
    EXPECT_FALSE(in_range(*one, "203.0.113.8"));
    const auto narrow = parse_ip_range("10.0.0.128/25");
    ASSERT_TRUE(narrow.has_value());
    EXPECT_TRUE(in_range(*narrow, "10.0.0.200"));
    EXPECT_FALSE(in_range(*narrow, "10.0.0.5"));
}

TEST(IpRange, RefusesNonsense) {
    EXPECT_FALSE(parse_ip_range("nope").has_value());
    EXPECT_FALSE(parse_ip_range("10.0.0.1/33").has_value());
    EXPECT_FALSE(parse_ip_range("::1/129").has_value());
    const auto any = parse_ip_range("0.0.0.0/0");
    ASSERT_TRUE(any.has_value());
    EXPECT_FALSE(in_range(*any, "not an ip"));
}

TEST(Scope, ExactPrefixAndStar) {
    ApiKey key{.accounts = {"binance", "okx-spot"}, .symbols = {"BTC/USDT", "ETH/*"}};
    EXPECT_TRUE(allows_account(key, "binance"));
    EXPECT_FALSE(allows_account(key, "bybit"));
    EXPECT_TRUE(allows_symbol(key, "BTC/USDT"));
    EXPECT_FALSE(allows_symbol(key, "BTC/USDC"));
    EXPECT_TRUE(allows_symbol(key, "ETH/USDC"));
    EXPECT_FALSE(all_accounts(key));
    EXPECT_FALSE(all_symbols(key));
    EXPECT_TRUE(allows_underlying(key, "ETH"));
    EXPECT_TRUE(allows_underlying(key, "BTC"));
    EXPECT_FALSE(allows_underlying(key, "ET"));
    EXPECT_FALSE(allows_underlying(key, "SOL"));
    const ApiKey none{};
    EXPECT_FALSE(allows_account(none, "binance"));
    EXPECT_FALSE(allows_symbol(none, "BTC/USDT"));
    const ApiKey star{.accounts = {"*"}, .symbols = {"*"}};
    EXPECT_TRUE(all_accounts(star));
    EXPECT_TRUE(allows_symbol(star, "anything"));
}

} // namespace
} // namespace raikiri::clientapi
