#include <raikiri/bookrisk/track.hpp>
#include <raikiri/oms/risk.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
} // namespace

// counted across the whole test binary; only the gate tests read it
// gcc pairs this malloc with the libc free below and reads a mismatch once they inline together
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* out = std::malloc(size); out != nullptr) {
        return out;
    }
    throw std::bad_alloc{};
}

void operator delete(void* at) noexcept {
    std::free(at);
}

void operator delete(void* at, std::size_t) noexcept {
    std::free(at);
}

namespace raikiri::bookrisk {
namespace {

using oms::BookState;
using oms::RiskReject;

constexpr InstrumentId k_btc = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{2});
constexpr InstrumentId k_eth = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{3});
constexpr std::uint64_t k_ms = 1'000'000;
constexpr std::uint64_t k_s = 1'000'000'000;

OrderCmd order(Side side = Side::Buy, std::int64_t qty = 1, std::uint64_t at = 1'000 * k_s, InstrumentId on = k_btc) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = side;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = on;
    cmd.client_order_id = ClientOrderId::make(7, 1);
    cmd.price = PriceTicks::from_raw(100);
    cmd.qty = QtyLots::from_raw(qty);
    cmd.ts_created = {at};
    return cmd;
}

RiskReject refused_for(const oms::RiskGate& gate, const OrderCmd& cmd, const oms::Exposure& exposure) {
    const auto got = gate.check(cmd, exposure);
    EXPECT_FALSE(got.has_value());
    return got.has_value() ? RiskReject::BadQty : got.error().why;
}

TEST(BookGate, NoCapitalRefusesEveryNewOrder) {
    const oms::RiskGate gate{};
    EXPECT_EQ(refused_for(gate, order(), {.capital_set = false}), RiskReject::NoCapital);
    EXPECT_TRUE(gate.check(order(), {}).has_value());
}

TEST(BookGate, HaltedRefusesAnOrderEvenOneThatWouldShrink) {
    const oms::RiskGate gate{};
    const oms::Exposure held{.position_lots = QtyLots::from_raw(5), .book_state = BookState::Halted};
    EXPECT_EQ(refused_for(gate, order(Side::Sell, 1), held), RiskReject::BookHalted);
}

TEST(BookGate, ReducingPassesOnlyWhatShrinksThePosition) {
    const oms::RiskGate gate{};
    oms::Exposure held{.position_lots = QtyLots::from_raw(5), .book_state = BookState::Reducing};
    EXPECT_TRUE(gate.check(order(Side::Sell, 5), held).has_value());
    EXPECT_EQ(refused_for(gate, order(Side::Sell, 6), held), RiskReject::BookReducing);
    EXPECT_EQ(refused_for(gate, order(Side::Buy, 1), held), RiskReject::BookReducing);

    // what already works on that side counts as filled, so two exits cannot flip the book
    held.working_sell_lots = QtyLots::from_raw(4);
    EXPECT_TRUE(gate.check(order(Side::Sell, 1), held).has_value());
    EXPECT_EQ(refused_for(gate, order(Side::Sell, 2), held), RiskReject::BookReducing);

    const oms::Exposure flat{.book_state = BookState::Reducing};
    EXPECT_EQ(refused_for(gate, order(Side::Sell, 1), flat), RiskReject::BookReducing);
    EXPECT_EQ(refused_for(gate, order(Side::Buy, 1), flat), RiskReject::BookReducing);

    const oms::Exposure shorted{.position_lots = QtyLots::from_raw(-3), .book_state = BookState::Reducing};
    EXPECT_TRUE(gate.check(order(Side::Buy, 3), shorted).has_value());
    EXPECT_EQ(refused_for(gate, order(Side::Buy, 4), shorted), RiskReject::BookReducing);
}

TEST(BookGate, OrderRateCountsTheSlidingWindow) {
    const oms::RiskGate gate{{.max_orders_in_window = 3, .order_window_ns = 1'000 * k_ms}};
    BookTrack track;
    const auto ask = [&](std::uint64_t at) {
        const auto cmd = order(Side::Buy, 1, at);
        oms::Exposure exposure{};
        track.fill(cmd, gate.config(), exposure);
        const auto verdict = gate.check(cmd, exposure);
        if (verdict.has_value()) {
            track.on_sent(cmd, exposure);
        }
        return verdict;
    };
    const std::uint64_t t0 = 500 * k_s;
    EXPECT_TRUE(ask(t0).has_value());
    EXPECT_TRUE(ask(t0 + 100 * k_ms).has_value());
    EXPECT_TRUE(ask(t0 + 200 * k_ms).has_value());
    const auto over = ask(t0 + 300 * k_ms);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().why, RiskReject::OrderRate);
    EXPECT_EQ(over.error().limit, 3);
    EXPECT_EQ(over.error().value, 4);
    // the first has aged out
    EXPECT_TRUE(ask(t0 + 1'001 * k_ms).has_value());
    EXPECT_FALSE(ask(t0 + 1'002 * k_ms).has_value());
}

TEST(BookGate, ARefusedOrderDoesNotCountAgainstTheRate) {
    const oms::RiskGate gate{{.max_orders_in_window = 1, .order_window_ns = k_s}};
    BookTrack track;
    oms::Exposure exposure{};
    const auto cmd = order();
    track.fill(cmd, gate.config(), exposure);
    EXPECT_EQ(exposure.orders_in_window, 0U);
}

TEST(BookGate, RepeatedExecutionStopsTheCycleAfterNReEntries) {
    const oms::RiskGate gate{{.max_repeat_entries = 2, .repeat_window_ns = 60 * k_s}};
    BookTrack track;
    std::uint64_t at = 1'000 * k_s;
    const auto enter = [&](const oms::Exposure& state) {
        const auto cmd = order(Side::Buy, 1, at += k_s);
        auto exposure = state;
        track.fill(cmd, gate.config(), exposure);
        const auto verdict = gate.check(cmd, exposure);
        if (verdict.has_value()) {
            track.on_sent(cmd, exposure);
        }
        return verdict;
    };
    EXPECT_TRUE(enter({}).has_value());
    EXPECT_TRUE(enter({}).has_value()) << "entering again with no fill between is no cycle";
    track.on_fill(k_btc);
    EXPECT_TRUE(enter({}).has_value());
    track.on_fill(k_btc);
    EXPECT_TRUE(enter({}).has_value());
    track.on_fill(k_btc);
    const auto third = enter({});
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error().why, RiskReject::RepeatedExecution);
    EXPECT_EQ(third.error().limit, 2);

    // the way out stays open: an order that only shrinks the book is no re-entry
    const auto cmd = order(Side::Sell, 1, at += k_s);
    oms::Exposure holding{.position_lots = QtyLots::from_raw(3)};
    track.fill(cmd, gate.config(), holding);
    EXPECT_TRUE(gate.check(cmd, holding).has_value());
}

TEST(BookGate, ReEntriesAreCountedPerInstrumentAndAgeOut) {
    const oms::RiskGate gate{{.max_repeat_entries = 1, .repeat_window_ns = 10 * k_s}};
    BookTrack track;
    const auto sent = [&](std::uint64_t at, InstrumentId on) {
        const auto cmd = order(Side::Buy, 1, at, on);
        oms::Exposure exposure{};
        track.fill(cmd, gate.config(), exposure);
        track.on_sent(cmd, exposure);
    };
    track.on_fill(k_btc);
    sent(1'000 * k_s, k_btc);
    oms::Exposure exposure{};
    track.fill(order(Side::Buy, 1, 1'001 * k_s, k_eth), gate.config(), exposure);
    EXPECT_FALSE(exposure.filled_since_entry);
    EXPECT_EQ(exposure.repeats_in_window, 1U);
    track.fill(order(Side::Buy, 1, 1'011 * k_s), gate.config(), exposure);
    EXPECT_EQ(exposure.repeats_in_window, 0U);
}

TEST(BookGate, AQuoteOlderThanTheLimitStopsEveryOrder) {
    const oms::RiskGate gate{{.max_quote_age_ns = 500 * k_ms}};
    const auto cmd = order(Side::Buy, 1, 1'000 * k_s);
    EXPECT_EQ(refused_for(gate, cmd, {}), RiskReject::StaleQuote) << "no quote at all is no fresher";
    EXPECT_TRUE(gate.check(cmd, {.quote_ts = {1'000 * k_s - 500 * k_ms}}).has_value());
    const auto old = gate.check(cmd, {.quote_ts = {1'000 * k_s - 501 * k_ms}});
    ASSERT_FALSE(old.has_value());
    EXPECT_EQ(old.error().why, RiskReject::StaleQuote);
    EXPECT_NE(oms::describe(old.error()).find("500 ms, would reach 501 ms"), std::string::npos)
        << oms::describe(old.error());
    // a quote stamped ahead of the order is not stale
    EXPECT_TRUE(gate.check(cmd, {.quote_ts = {1'001 * k_s}}).has_value());
    // off by default
    EXPECT_TRUE(oms::RiskGate{}.check(cmd, {}).has_value());
}

TEST(BookGate, TheCheckAndTheCountsDoNotAllocate) {
    const oms::RiskGate gate{{.max_orders_in_window = 500, .max_repeat_entries = 500, .max_quote_age_ns = 10 * k_s}};
    auto track = std::make_unique<BookTrack>();
    const auto before = g_allocations.load();
    std::uint64_t passed = 0;
    for (std::uint64_t i = 0; i < 2'000; ++i) {
        const auto cmd = order(i % 2 == 0 ? Side::Buy : Side::Sell, 1, (1'000 * k_s) + (i * k_s / 100));
        oms::Exposure exposure{.quote_ts = cmd.ts_created};
        track->fill(cmd, gate.config(), exposure);
        if (gate.check(cmd, exposure).has_value()) {
            track->on_sent(cmd, exposure);
            ++passed;
        }
        track->on_fill(cmd.instrument);
    }
    EXPECT_EQ(g_allocations.load(), before);
    EXPECT_GT(passed, 0U);
}

struct Step {
    enum class Kind : std::uint8_t { Order, Fill } kind{Kind::Order};
    OrderCmd cmd{};
    oms::Exposure exposure{};
};

std::vector<int> replay(const oms::RiskGate& gate, const std::vector<Step>& script, BookState state) {
    BookTrack track;
    std::vector<int> verdicts;
    for (const auto& step : script) {
        if (step.kind == Step::Kind::Fill) {
            track.on_fill(step.cmd.instrument);
            continue;
        }
        auto exposure = step.exposure;
        exposure.book_state = state;
        track.fill(step.cmd, gate.config(), exposure);
        const auto verdict = gate.check(step.cmd, exposure);
        if (verdict.has_value()) {
            track.on_sent(step.cmd, exposure);
            verdicts.push_back(-1);
        } else {
            verdicts.push_back(static_cast<int>(verdict.error().why));
        }
    }
    return verdicts;
}

TEST(BookGate, ReplayingTheSameStreamReachesTheSameVerdicts) {
    const oms::RiskConfig config{.max_orders_in_window = 4,
                                 .order_window_ns = k_s,
                                 .max_repeat_entries = 3,
                                 .repeat_window_ns = 5 * k_s,
                                 .max_quote_age_ns = 700 * k_ms};
    std::vector<Step> script;
    std::uint64_t at = 1'000 * k_s;
    for (std::uint32_t i = 0; i < 400; ++i) {
        at += (i * 37 % 11) * 15 * k_ms;
        if (i % 3 == 0) {
            script.push_back({.kind = Step::Kind::Fill, .cmd = order(Side::Buy, 1, at, i % 6 == 0 ? k_btc : k_eth)});
            continue;
        }
        script.push_back(
            {.cmd = order(i % 5 == 0 ? Side::Sell : Side::Buy, 1 + (i % 3), at, i % 4 == 0 ? k_eth : k_btc),
             .exposure = {.position_lots = QtyLots::from_raw(static_cast<std::int64_t>(i % 7) - 3),
                          .quote_ts = {at - ((i * 13 % 9) * 100 * k_ms)}}});
    }
    const oms::RiskGate one{config};
    const oms::RiskGate two{config};
    for (const auto state : {BookState::Active, BookState::Reducing, BookState::Halted}) {
        const auto first = replay(one, script, state);
        EXPECT_EQ(first, replay(two, script, state));
        EXPECT_FALSE(first.empty());
    }
    const auto active = replay(one, script, BookState::Active);
    const auto count = [&](RiskReject why) { return std::ranges::count(active, static_cast<int>(why)); };
    EXPECT_GT(count(RiskReject::OrderRate), 0) << "the script should reach each new check";
    EXPECT_GT(count(RiskReject::StaleQuote), 0);
    EXPECT_GT(count(RiskReject::RepeatedExecution), 0);
    EXPECT_GT(std::ranges::count(active, -1), 0);
}

} // namespace
} // namespace raikiri::bookrisk
