#include <raikiri/internalcross/domain/match.hpp>
#include <raikiri/internalcross/domain/settings.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size); block != nullptr) {
        return block;
    }
    throw std::bad_alloc{};
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace raikiri::internalcross {
namespace {

constexpr UnixNanos k_now{1'000'000'000'000'000'000ULL};
constexpr QuotePolicy k_policy{.max_quote_age_ns = 1'000'000'000, .max_spread_bp = 20};
const FixedE9 k_tick = FixedE9::from_raw(100'000'000); // 0.1

FixedE9 price(std::int64_t whole_tenths) {
    return FixedE9::from_raw(whole_tenths * 100'000'000);
}

Reference touch(std::int64_t bid_tenths, std::int64_t ask_tenths) {
    return {.bid = price(bid_tenths), .ask = price(ask_tenths)};
}

Interest buy(std::uint64_t seq, std::uint16_t book, std::int64_t lots, std::int64_t limit_ticks = 0) {
    return {.order = ClientOrderId::make(book, seq),
            .book = book,
            .side = Side::Buy,
            .limit = limit_ticks == 0 ? PriceTicks::undef() : PriceTicks::from_raw(limit_ticks),
            .leaves = QtyLots::from_raw(lots)};
}

Interest sell(std::uint64_t seq, std::uint16_t book, std::int64_t lots, std::int64_t limit_ticks = 0) {
    auto out = buy(seq, book, lots, limit_ticks);
    out.side = Side::Sell;
    return out;
}

constexpr auto k_anyone = [](std::uint16_t, ClientOrderId, Side, QtyLots, PriceTicks) { return true; };

TEST(BestTouch, TakesTheBestLiveVenue) {
    const std::array touches{Touch{.bid = price(1000), .ask = price(1004), .ts = k_now, .own = true},
                             Touch{.bid = price(1001), .ask = price(1003), .ts = k_now},
                             Touch{.bid = price(1000), .ask = price(1005), .ts = k_now}};
    const auto ref = best_touch(touches, k_policy, k_now);
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->bid.raw, price(1001).raw);
    EXPECT_EQ(ref->ask.raw, price(1003).raw);
}

TEST(BestTouch, LeavesOutAStaleVenueButRefusesAStaleOwnBook) {
    const UnixNanos old{k_now.ns - 2'000'000'000};
    const std::array other_stale{Touch{.bid = price(100'000), .ask = price(100'004), .ts = k_now, .own = true},
                                 Touch{.bid = price(100'002), .ask = price(100'003), .ts = old}};
    EXPECT_EQ(best_touch(other_stale, k_policy, k_now)->bid.raw, price(100'000).raw);
    const std::array own_stale{Touch{.bid = price(100'000), .ask = price(100'004), .ts = old, .own = true},
                               Touch{.bid = price(100'002), .ask = price(100'003), .ts = k_now}};
    EXPECT_EQ(best_touch(own_stale, k_policy, k_now).error(), Refusal::StaleQuote);
}

TEST(BestTouch, RefusesNoQuoteCrossedAndWide) {
    const std::array unquoted{Touch{.ts = k_now, .own = true}};
    EXPECT_EQ(best_touch(unquoted, k_policy, k_now).error(), Refusal::NoQuote);
    const std::array nobody{Touch{.bid = price(1000), .ask = price(1001), .ts = k_now}};
    EXPECT_EQ(best_touch(nobody, k_policy, k_now).error(), Refusal::NoQuote);
    const std::array crossed{Touch{.bid = price(1000), .ask = price(1001), .ts = k_now, .own = true},
                             Touch{.bid = price(1002), .ask = price(1003), .ts = k_now}};
    EXPECT_EQ(best_touch(crossed, k_policy, k_now).error(), Refusal::CrossedQuote);
    const std::array locked{Touch{.bid = price(1000), .ask = price(1000), .ts = k_now, .own = true}};
    EXPECT_EQ(best_touch(locked, k_policy, k_now).error(), Refusal::CrossedQuote);
    // 21 bp over 20
    const std::array wide{Touch{.bid = price(100'000), .ask = price(100'210), .ts = k_now, .own = true}};
    EXPECT_EQ(best_touch(wide, k_policy, k_now).error(), Refusal::WideQuote);
    const std::array fine{Touch{.bid = price(100'000), .ask = price(100'200), .ts = k_now, .own = true}};
    EXPECT_TRUE(best_touch(fine, k_policy, k_now).has_value());
}

TEST(MidOnGrid, ExactOffGridAndTies) {
    EXPECT_EQ(mid_on_grid(touch(1000, 1004), k_tick, Rounding::Nearest)->raw, 1002);
    // a one-tick book has its mid half a tick off the grid
    EXPECT_EQ(mid_on_grid(touch(1000, 1001), k_tick, Rounding::Down)->raw, 1000);
    EXPECT_EQ(mid_on_grid(touch(1000, 1001), k_tick, Rounding::Up)->raw, 1001);
    EXPECT_EQ(mid_on_grid(touch(1000, 1001), k_tick, Rounding::Nearest)->raw, 1000);
    EXPECT_EQ(mid_on_grid(touch(1001, 1002), k_tick, Rounding::Nearest)->raw, 1002);
}

TEST(MidOnGrid, NoTickInsideTheTouchIsRefused) {
    // venues on a finer grid than this listing: 100.01 to 100.03 holds no 0.1 tick
    const Reference ref{.bid = FixedE9::from_raw(100'010'000'000), .ask = FixedE9::from_raw(100'030'000'000)};
    EXPECT_FALSE(mid_on_grid(ref, k_tick, Rounding::Nearest).has_value());
    EXPECT_FALSE(mid_on_grid(ref, FixedE9::from_raw(0), Rounding::Nearest).has_value());
}

TEST(Plan, ClientsCrossAtTheMidInsideBothLimits) {
    const auto arriving = buy(1, 10, 5, 1003);
    const std::array opposite{sell(2, 20, 5, 1001)};
    std::array<Allocation, 4> out{};
    const auto plan = plan_cross(
        arriving, TimeInForce::Gtc, opposite, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    ASSERT_EQ(plan.count, 1U);
    EXPECT_EQ(plan.crossed.raw, 5);
    EXPECT_EQ(out[0].price.raw, 1002);
    EXPECT_EQ(out[0].counter, opposite[0].order);
    EXPECT_FALSE(out[0].desk);
}

TEST(Plan, NeverWorseThanEitherClientsTouchNorLimit) {
    std::array<Allocation, 4> out{};
    const auto ref = touch(1000, 1004);
    const auto low_limit = buy(1, 10, 5, 1001);
    const std::array opposite{sell(2, 20, 5)};
    auto plan = plan_cross(low_limit, TimeInForce::Gtc, opposite, std::nullopt, ref, k_tick, k_anyone, out);
    EXPECT_EQ(plan.count, 0U);
    EXPECT_EQ(plan.why, Refusal::OutsideLimit);
    const std::array high_ask{sell(2, 20, 5, 1003)};
    plan = plan_cross(buy(1, 10, 5), TimeInForce::Gtc, high_ask, std::nullopt, ref, k_tick, k_anyone, out);
    EXPECT_EQ(plan.why, Refusal::OutsideLimit);
    // inside the touch, whatever the market's direction
    plan = plan_cross(buy(1, 10, 5), TimeInForce::Gtc, opposite, std::nullopt, ref, k_tick, k_anyone, out);
    ASSERT_EQ(plan.count, 1U);
    EXPECT_GE(out[0].price.raw, 1000);
    EXPECT_LE(out[0].price.raw, 1004);
}

TEST(Plan, AFeeThatPassesTheTouchRefuses) {
    auto arriving = buy(1, 10, 5);
    arriving.fee_bp = 10; // 10 bp over a 2 bp half spread
    const std::array opposite{sell(2, 20, 5)};
    std::array<Allocation, 4> out{};
    const auto ref = Reference{.bid = price(100'000), .ask = price(100'040)};
    auto plan = plan_cross(arriving, TimeInForce::Gtc, opposite, std::nullopt, ref, k_tick, k_anyone, out);
    EXPECT_EQ(plan.why, Refusal::FeeOutsideTouch);
    arriving.fee_bp = 1;
    plan = plan_cross(arriving, TimeInForce::Gtc, opposite, std::nullopt, ref, k_tick, k_anyone, out);
    EXPECT_EQ(plan.count, 1U);
}

TEST(Plan, PartialThenOldestFirstAndTheRestLeftOver) {
    const auto arriving = buy(1, 10, 8);
    const std::array opposite{sell(2, 20, 3), sell(3, 30, 3), sell(4, 40, 7)};
    std::array<Allocation, 4> out{};
    const auto plan = plan_cross(
        arriving, TimeInForce::Gtc, opposite, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    ASSERT_EQ(plan.count, 3U);
    EXPECT_EQ(plan.crossed.raw, 8);
    EXPECT_EQ(out[0].qty.raw, 3);
    EXPECT_EQ(out[1].qty.raw, 3);
    EXPECT_EQ(out[2].qty.raw, 2);
    const std::array short_book{sell(2, 20, 3)};
    const auto part = plan_cross(
        arriving, TimeInForce::Gtc, short_book, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    EXPECT_EQ(part.crossed.raw, 3);
}

TEST(Plan, NoSelfCrossInOneBook) {
    const auto arriving = buy(1, 10, 5);
    const std::array opposite{sell(2, 10, 5)};
    std::array<Allocation, 4> out{};
    const auto plan = plan_cross(
        arriving, TimeInForce::Gtc, opposite, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    EXPECT_EQ(plan.count, 0U);
    EXPECT_EQ(plan.why, Refusal::NoCounterparty);
    const Inventory own{.book = 10, .position = QtyLots::from_raw(5)};
    EXPECT_EQ(plan_cross(arriving, TimeInForce::Gtc, {}, own, touch(1000, 1004), k_tick, k_anyone, out).count, 0U);
}

TEST(Plan, FillOrKillCrossesWholeOrNotAtAll) {
    const auto arriving = buy(1, 10, 5);
    const std::array opposite{sell(2, 20, 3)};
    std::array<Allocation, 4> out{};
    auto plan = plan_cross(
        arriving, TimeInForce::Fok, opposite, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    EXPECT_EQ(plan.count, 0U);
    EXPECT_EQ(plan.crossed.raw, 0);
    EXPECT_EQ(plan.why, Refusal::FokShort);
    const std::array whole{sell(2, 20, 7)};
    plan = plan_cross(arriving, TimeInForce::Fok, whole, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    EXPECT_EQ(plan.crossed.raw, 5);
}

TEST(Plan, TheDeskOnlyEverShrinks) {
    std::array<Allocation, 4> out{};
    const auto ref = touch(1000, 1001);
    const Inventory longs{.book = 99, .position = QtyLots::from_raw(4), .working_sell = QtyLots::from_raw(1)};
    auto plan = plan_cross(buy(1, 10, 9), TimeInForce::Gtc, {}, longs, ref, k_tick, k_anyone, out);
    ASSERT_EQ(plan.count, 1U);
    EXPECT_TRUE(out[0].desk);
    EXPECT_EQ(out[0].qty.raw, 3);      // its working sell already spends the rest
    EXPECT_EQ(out[0].price.raw, 1000); // half a tick goes to the client
    // a client selling into a long desk would grow it
    plan = plan_cross(sell(1, 10, 9), TimeInForce::Gtc, {}, longs, ref, k_tick, k_anyone, out);
    EXPECT_EQ(plan.count, 0U);
    const Inventory shorts{.book = 99, .position = QtyLots::from_raw(-4)};
    plan = plan_cross(sell(1, 10, 9), TimeInForce::Gtc, {}, shorts, ref, k_tick, k_anyone, out);
    ASSERT_EQ(plan.count, 1U);
    EXPECT_EQ(out[0].qty.raw, 4);
    EXPECT_EQ(out[0].price.raw, 1001);
}

TEST(Plan, ClientsGoBeforeTheDesk) {
    std::array<Allocation, 4> out{};
    const std::array opposite{sell(2, 20, 2)};
    const Inventory longs{.book = 99, .position = QtyLots::from_raw(4)};
    const auto plan = plan_cross(
        buy(1, 10, 3), TimeInForce::Gtc, opposite, longs, touch(1000, 1004), k_tick, k_anyone, out);
    ASSERT_EQ(plan.count, 2U);
    EXPECT_FALSE(out[0].desk);
    EXPECT_EQ(out[0].qty.raw, 2);
    EXPECT_TRUE(out[1].desk);
    EXPECT_EQ(out[1].qty.raw, 1);
}

TEST(Plan, ARefusedCounterpartyIsPassedOver) {
    std::array<Allocation, 4> out{};
    const std::array opposite{sell(2, 20, 2), sell(3, 30, 2)};
    const auto only_30 = [](std::uint16_t book, ClientOrderId, Side, QtyLots, PriceTicks) { return book == 30; };
    const auto plan = plan_cross(
        buy(1, 10, 5), TimeInForce::Gtc, opposite, std::nullopt, touch(1000, 1004), k_tick, only_30, out);
    ASSERT_EQ(plan.count, 1U);
    EXPECT_EQ(out[0].book, 30);
    EXPECT_EQ(plan.why, Refusal::Risk);
}

TEST(Plan, ASpotSellerCannotGoShortInside) {
    std::array<Allocation, 4> out{};
    auto arriving = sell(1, 10, 5);
    arriving.sell_cap = QtyLots::from_raw(2);
    const std::array opposite{buy(2, 20, 5)};
    auto plan = plan_cross(
        arriving, TimeInForce::Gtc, opposite, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    EXPECT_EQ(plan.crossed.raw, 2);
    auto seller = sell(2, 20, 5);
    seller.sell_cap = QtyLots::from_raw(0);
    const std::array empty_handed{seller};
    plan = plan_cross(
        buy(1, 10, 5), TimeInForce::Gtc, empty_handed, std::nullopt, touch(1000, 1004), k_tick, k_anyone, out);
    EXPECT_EQ(plan.count, 0U);
}

TEST(Plan, TheSameInputsPlanTheSame) {
    const std::array opposite{sell(2, 20, 3), sell(3, 30, 3)};
    std::array<Allocation, 4> first{};
    std::array<Allocation, 4> second{};
    const auto one = plan_cross(
        buy(1, 10, 5), TimeInForce::Gtc, opposite, std::nullopt, touch(1000, 1005), k_tick, k_anyone, first);
    const auto two = plan_cross(
        buy(1, 10, 5), TimeInForce::Gtc, opposite, std::nullopt, touch(1000, 1005), k_tick, k_anyone, second);
    ASSERT_EQ(one.count, two.count);
    for (std::size_t i = 0; i < one.count; ++i) {
        EXPECT_EQ(first[i].price, second[i].price);
        EXPECT_EQ(first[i].qty, second[i].qty);
        EXPECT_EQ(first[i].counter, second[i].counter);
    }
}

TEST(Plan, DoesNotAllocate) {
    const std::array touches{Touch{.bid = price(1000), .ask = price(1004), .ts = k_now, .own = true},
                             Touch{.bid = price(1001), .ask = price(1003), .ts = k_now}};
    const std::array opposite{sell(2, 20, 3), sell(3, 30, 3)};
    const Inventory longs{.book = 99, .position = QtyLots::from_raw(4)};
    std::array<Allocation, 4> out{};
    const auto before = g_allocations.load();
    std::size_t crossed = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto ref = best_touch(touches, k_policy, k_now);
        crossed += plan_cross(buy(1, 10, 9), TimeInForce::Gtc, opposite, longs, *ref, k_tick, k_anyone, out).count;
        (void)venue_cost_saved(*ref, price(1000), FixedE9::from_raw(500'000));
    }
    EXPECT_EQ(g_allocations.load(), before);
    EXPECT_EQ(crossed, 3000U);
}

TEST(Settings, OffByDefaultAndOnlyWhatIsListed) {
    const Settings settings{};
    EXPECT_FALSE(settings.enabled);
    EXPECT_TRUE(validate(settings).empty());
    const BookPolicy policy{.enabled = true, .instruments = {"BTC/USDT:perp"}};
    EXPECT_TRUE(allows(policy, "BTC/USDT", MarketType::Perp));
    EXPECT_FALSE(allows(policy, "BTC/USDT", MarketType::Spot));
    EXPECT_FALSE(allows(policy, "ETH/USDT", MarketType::Perp));
    EXPECT_FALSE(allows(BookPolicy{.enabled = true}, "BTC/USDT", MarketType::Perp));
}

TEST(Settings, RefusesAWaitLongerThanReconciliationGrants) {
    EXPECT_FALSE(validate({.hold_ms = 5000}).empty());
    EXPECT_FALSE(validate({.max_spread_bp = 0}).empty());
    EXPECT_FALSE(validate({.books = {{"hedge", {}}}}).empty());
    EXPECT_FALSE(validate({.books = {{"client:a", {.instruments = {"BTC/USDT"}}}}}).empty());
    EXPECT_TRUE(validate({.hold_ms = 100, .books = {{"client:a", {.instruments = {"BTC/USDT:perp"}}}}}).empty());
}

TEST(Settings, NamesWhatLetsMoreThrough) {
    const Settings before{};
    const Settings on{.enabled = true, .books = {{"client:a", {.enabled = true, .instruments = {"BTC/USDT:perp"}}}}};
    EXPECT_EQ(raises(before, on).size(), 2U);
    EXPECT_TRUE(raises(on, before).empty());
}

} // namespace
} // namespace raikiri::internalcross
