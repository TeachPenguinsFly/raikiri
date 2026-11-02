#include <raikiri/bookrisk/desk.hpp>

#include <gtest/gtest.h>

#include "fakes.hpp"

namespace raikiri::bookrisk {
namespace {

using namespace raikiri::bookrisk::test;
using oms::BookState;
using oms::RiskReject;

constexpr std::uint64_t k_s = 1'000'000'000;
constexpr BookId k_book = 7;
constexpr std::uint64_t k_start = 1'800'000'000 * k_s;

ChangeRequest vlad(bool confirm = false) {
    return {.by = "vlad", .reason = "test", .confirm = confirm};
}

LimitPatch one(Param param, LimitMode mode, std::string_view value = {}) {
    return {.limits = {{param, Limit{.mode = mode, .value = value.empty() ? FixedE9::undef() : d(value)}}}};
}

class Desk : public ::testing::Test {
protected:
    FakeKernel kernel;
    MemoryStore store;
    MemoryAudit audit;
    MemoryNotifier notices;
    TestClock clock{UnixNanos{k_start}};
    std::unique_ptr<RiskDesk> desk{std::make_unique<RiskDesk>(kernel, store, audit, notices, clock)};

    void restart() {
        desk = std::make_unique<RiskDesk>(kernel, store, audit, notices, clock);
        ASSERT_TRUE(desk->load().has_value());
    }

    void capital(std::string_view dollars = "100000") {
        ASSERT_TRUE(desk->set_capital(d(dollars), vlad(true)).has_value());
    }

    [[nodiscard]] std::expected<void, oms::RiskRefusal> ask(const OrderCmd& cmd, oms::Exposure exposure = {}) {
        desk->admit(k_book, cmd, exposure);
        const auto verdict = desk->gate(k_book).check(cmd, exposure);
        if (verdict.has_value()) {
            desk->sent(k_book, cmd, exposure);
        } else {
            desk->refused(k_book, verdict.error());
        }
        return verdict;
    }

    [[nodiscard]] FixedE9 effective(Param param) {
        return desk->book_view(std::to_string(k_book))->row.limits[static_cast<std::size_t>(param)].resolved.effective;
    }

    void seconds(std::uint64_t n, const std::function<void(std::uint64_t)>& each = {}) {
        for (std::uint64_t i = 0; i < n; ++i) {
            clock.advance(k_s);
            if (each) {
                each(i);
            }
            desk->tick();
        }
    }
};

TEST_F(Desk, WithNoCapitalNewOrdersAreRefusedAndAKillStillWorks) {
    const auto verdict = ask(buy(k_start));
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error().why, RiskReject::NoCapital);
    kernel.open[k_book] = 2;
    const auto killed = desk->kill(k_book, vlad());
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(*killed, 2U);
    EXPECT_EQ(kernel.cancels.size(), 1U);
}

TEST_F(Desk, SettingCapitalLetsOrdersThroughWithoutARestart) {
    EXPECT_FALSE(ask(buy(k_start)).has_value());
    capital();
    EXPECT_TRUE(ask(buy(k_start)).has_value());
}

TEST_F(Desk, FirstCapitalIsARaiseAndNeedsTheConfirmFlag) {
    const auto refused = desk->set_capital(d("100000"), vlad());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().error, DeskError::NeedsConfirm);
    EXPECT_EQ(refused.error().raises.front(), "capital");
    EXPECT_FALSE(store.saved.has_value());
    EXPECT_TRUE(desk->view().capital.is_undef());
    EXPECT_TRUE(desk->set_capital(d("100000"), vlad(true)).has_value());
}

TEST_F(Desk, AChangeNamesWhoMadeItAndIsRefusedWithoutOne) {
    const auto refused = desk->set_capital(d("100000"), {.confirm = true});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().error, DeskError::Invalid);
    EXPECT_TRUE(audit.rows.empty());
}

TEST_F(Desk, EveryChangeReachesTheAuditWithWhoOldAndNew) {
    capital("100000");
    ASSERT_EQ(audit.rows.size(), 1U);
    EXPECT_EQ(audit.rows[0].actor, "vlad");
    EXPECT_EQ(audit.rows[0].action, "risk.capital.set");
    EXPECT_NE(audit.rows[0].detail.find(R"("old":null,"new":"100000")"), std::string::npos) << audit.rows[0].detail;

    ASSERT_TRUE(desk->set_capital(d("80000"), vlad()).has_value());
    EXPECT_NE(audit.rows[1].detail.find(R"("old":"100000","new":"80000")"), std::string::npos) << audit.rows[1].detail;

    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Percent, "2"), vlad(true)).has_value());
    EXPECT_EQ(audit.rows[2].action, "risk.book.limits.set");
    EXPECT_EQ(audit.rows[2].subject, "book:7");
    EXPECT_NE(audit.rows[2].detail.find(
                  R"("old":{"order_notional":"inherit"},"new":{"order_notional":{"mode":"percent","value":"2"}})"),
              std::string::npos)
        << audit.rows[2].detail;

    ASSERT_TRUE(desk->set_defaults(one(Param::QuoteAgeMs, LimitMode::Absolute, "5000"), vlad()).has_value());
    EXPECT_EQ(audit.rows[3].action, "risk.defaults.set");
    EXPECT_EQ(audit.rows[3].subject, "defaults");

    ASSERT_TRUE(desk->set_state(k_book, BookState::Reducing, vlad()).has_value());
    EXPECT_EQ(audit.rows[4].action, "risk.book.state.set");
    EXPECT_NE(audit.rows[4].detail.find(R"("old":"active","new":"reducing")"), std::string::npos);
}

TEST_F(Desk, APercentIsShownInDollarsAndADollarInPercents) {
    capital("200000");
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Percent, "2.5"), vlad(true)).has_value());
    ASSERT_TRUE(
        desk->set_limits(k_book, one(Param::BookNotional, LimitMode::Absolute, "50000"), vlad(true)).has_value());
    const auto book = desk->book_view("7");
    const auto& order = book->row.limits[static_cast<std::size_t>(Param::OrderNotional)];
    EXPECT_EQ(order.set.mode, LimitMode::Percent);
    EXPECT_EQ(order.resolved.effective, d("5000"));
    EXPECT_EQ(order.resolved.percent, d("2.5"));
    EXPECT_EQ(order.resolved.source, LimitSource::Book);
    const auto& whole = book->row.limits[static_cast<std::size_t>(Param::BookNotional)];
    EXPECT_EQ(whole.set.mode, LimitMode::Absolute);
    EXPECT_EQ(whole.resolved.effective, d("50000"));
    EXPECT_EQ(whole.resolved.percent, d("25"));
}

TEST_F(Desk, ALimitChangeAppliesToTheGateLive) {
    capital();
    const auto& gate = desk->gate(k_book);
    EXPECT_TRUE(gate.config().max_order_notional.is_undef());
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Percent, "1"), vlad(true)).has_value());
    EXPECT_EQ(gate.config().max_order_notional, d("1000"));
    // the capital moves a percent limit with it
    ASSERT_TRUE(desk->set_capital(d("50000"), vlad()).has_value());
    EXPECT_EQ(gate.config().max_order_notional, d("500"));
}

TEST_F(Desk, TheBookRowBeatsDefaultsAndDefaultsBeatTheFile) {
    kernel.file.max_order_notional = d("20");
    capital();
    EXPECT_EQ(effective(Param::OrderNotional), d("20"));
    ASSERT_TRUE(desk->set_defaults(one(Param::OrderNotional, LimitMode::Absolute, "10"), vlad()).has_value());
    EXPECT_EQ(effective(Param::OrderNotional), d("10"));
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Absolute, "5"), vlad()).has_value());
    EXPECT_EQ(effective(Param::OrderNotional), d("5"));
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Inherit), vlad(true)).has_value());
    EXPECT_EQ(effective(Param::OrderNotional), d("10"));
}

TEST_F(Desk, ARaiseIsRefusedWithoutConfirmAndALoweringIsNot) {
    capital();
    ASSERT_TRUE(
        desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Absolute, "1000"), vlad(true)).has_value());
    const auto saves = store.saves;

    const auto higher = desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Absolute, "2000"), vlad());
    ASSERT_FALSE(higher.has_value());
    EXPECT_EQ(higher.error().error, DeskError::NeedsConfirm);
    EXPECT_EQ(higher.error().raises, std::vector<std::string>{"order_notional"});
    EXPECT_EQ(store.saves, saves);
    EXPECT_EQ(effective(Param::OrderNotional), d("1000"));

    EXPECT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Absolute, "500"), vlad()).has_value());
    const auto off = desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Off), vlad());
    ASSERT_FALSE(off.has_value()) << "turning a limit off is the largest raise there is";
    EXPECT_EQ(off.error().error, DeskError::NeedsConfirm);
    EXPECT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Off), vlad(true)).has_value());
}

TEST_F(Desk, AShorterWindowCountsAsARaise) {
    capital();
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderWindowMs, LimitMode::Absolute, "2000"), vlad()).has_value())
        << "the default window is one second, so two is a tightening";
    EXPECT_FALSE(desk->set_limits(k_book, one(Param::OrderWindowMs, LimitMode::Absolute, "500"), vlad()).has_value());
    EXPECT_TRUE(
        desk->set_limits(k_book, one(Param::OrderWindowMs, LimitMode::Absolute, "500"), vlad(true)).has_value());
}

TEST_F(Desk, ACapitalRaiseNamesThePercentLimitsItLifts) {
    capital();
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Percent, "1"), vlad(true)).has_value());
    const auto refused = desk->set_capital(d("200000"), vlad());
    ASSERT_FALSE(refused.has_value());
    const std::vector<std::string> want{"capital", "order_notional"};
    EXPECT_EQ(refused.error().raises, want);
}

TEST_F(Desk, TurningOffTheDailyLossActionIsARaise) {
    capital();
    LimitPatch patch{.loss_action = LossAction::Halted};
    ASSERT_TRUE(desk->set_limits(k_book, patch, vlad()).has_value());
    patch.loss_action = LossAction::Off;
    const auto refused = desk->set_limits(k_book, patch, vlad());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().raises, std::vector<std::string>{"daily_loss_action"});
}

TEST_F(Desk, BadValuesAreRefusedAndNothingIsSaved) {
    capital();
    const auto saves = store.saves;
    EXPECT_FALSE(desk->set_limits(k_book, one(Param::OpenOrders, LimitMode::Percent, "1"), vlad(true)).has_value());
    EXPECT_FALSE(desk->set_limits(k_book, one(Param::OpenOrders, LimitMode::Off), vlad(true)).has_value());
    EXPECT_FALSE(desk->set_limits(k_book, one(Param::OpenOrders, LimitMode::Absolute, "1.5"), vlad(true)).has_value());
    EXPECT_FALSE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Absolute, "0"), vlad(true)).has_value());
    EXPECT_FALSE(
        desk->set_limits(k_book, one(Param::OrdersInWindow, LimitMode::Absolute, "5000"), vlad(true)).has_value());
    EXPECT_FALSE(desk->set_capital(d("0"), vlad(true)).has_value());
    EXPECT_EQ(store.saves, saves);
}

TEST_F(Desk, AChangeThatCannotBeSavedIsNotApplied) {
    capital();
    store.fail = true;
    const auto refused = desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Absolute, "500"), vlad());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().error, DeskError::NotDurable);
    EXPECT_TRUE(effective(Param::OrderNotional).is_undef());
}

TEST_F(Desk, KillHaltsThatBookAndCancelsOnlyItsOrders) {
    capital();
    kernel.books = {7, 8};
    kernel.open = {{7, 3}, {8, 4}};
    const auto killed = desk->kill(k_book, vlad());
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(*killed, 3U);
    ASSERT_EQ(kernel.cancels.size(), 1U);
    EXPECT_EQ(kernel.cancels[0], std::optional<BookId>{7});
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);
    EXPECT_EQ(desk->book_view("8")->state, BookState::Active);

    const auto verdict = ask(buy(k_start));
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error().why, RiskReject::BookHalted);

    EXPECT_EQ(audit.rows.back().action, "risk.book.kill");
    EXPECT_EQ(audit.rows.back().actor, "vlad");
    EXPECT_NE(audit.rows.back().detail.find(R"("old":"active","new":"halted")"), std::string::npos);
    EXPECT_NE(audit.rows.back().detail.find(R"("cancelled":3)"), std::string::npos);
    EXPECT_EQ(notices.count("book_halted"), 1U);
}

TEST_F(Desk, KillEverythingHaltsEveryBookAndCancelsEveryOrder) {
    capital();
    kernel.books = {7, 8};
    kernel.open = {{7, 3}, {8, 4}};
    const auto killed = desk->kill(std::nullopt, vlad());
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(*killed, 7U);
    EXPECT_EQ(kernel.cancels.front(), std::nullopt);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);
    EXPECT_EQ(desk->book_view("8")->state, BookState::Halted);
    EXPECT_EQ(desk->book_view("7")->own.state, BookState::Active);
    EXPECT_EQ(desk->view().desk.state, BookState::Halted);
    EXPECT_EQ(audit.rows.back().action, "risk.desk.kill");
    // a book nobody has met yet is covered too
    oms::Exposure exposure{};
    desk->admit(99, buy(k_start), exposure);
    EXPECT_EQ(exposure.book_state, BookState::Halted);
}

TEST_F(Desk, ResumeTakesAPersonAndTheConfirmFlag) {
    capital();
    ASSERT_TRUE(desk->kill(k_book, vlad()).has_value());
    EXPECT_FALSE(desk->set_state(k_book, BookState::Active, {.confirm = true}).has_value());
    const auto unconfirmed = desk->set_state(k_book, BookState::Active, vlad());
    ASSERT_FALSE(unconfirmed.has_value());
    EXPECT_EQ(unconfirmed.error().error, DeskError::NeedsConfirm);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);
    seconds(120);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted) << "time does not resume a book";

    ASSERT_TRUE(desk->set_state(k_book, BookState::Active, vlad(true)).has_value());
    EXPECT_EQ(desk->book_view("7")->state, BookState::Active);
    EXPECT_NE(audit.rows.back().detail.find(R"("old":"halted","new":"active")"), std::string::npos);

    ASSERT_TRUE(desk->kill(std::nullopt, vlad()).has_value());
    EXPECT_FALSE(desk->resume_desk(vlad()).has_value());
    EXPECT_FALSE(desk->resume_desk({.confirm = true}).has_value());
    ASSERT_TRUE(desk->resume_desk(vlad(true)).has_value());
    EXPECT_EQ(desk->view().desk.state, BookState::Active);
}

TEST_F(Desk, ResumingTheDeskLeavesABookHaltedOnItsOwnHalted) {
    capital();
    ASSERT_TRUE(desk->kill(k_book, vlad()).has_value());
    ASSERT_TRUE(desk->kill(std::nullopt, vlad()).has_value());
    ASSERT_TRUE(desk->resume_desk(vlad(true)).has_value());
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);
}

TEST_F(Desk, AHaltIsRefusedThroughTheStateRouteSoItAlwaysPullsTheOrders) {
    capital();
    const auto refused = desk->set_state(k_book, BookState::Halted, vlad());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().error, DeskError::Invalid);
}

TEST_F(Desk, ReducingLetsOnlyShrinkingOrdersOut) {
    capital();
    ASSERT_TRUE(desk->set_state(k_book, BookState::Reducing, vlad()).has_value());
    const auto grow = ask(buy(k_start), {.position_lots = QtyLots::from_raw(5)});
    ASSERT_FALSE(grow.has_value());
    EXPECT_EQ(grow.error().why, RiskReject::BookReducing);
    EXPECT_TRUE(ask(buy(k_start, 2, Side::Sell), {.position_lots = QtyLots::from_raw(5)}).has_value());
}

TEST_F(Desk, AHaltSurvivesARestartAndSoDoesTheRest) {
    capital("150000");
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::OrderNotional, LimitMode::Percent, "2"), vlad(true)).has_value());
    ASSERT_TRUE(desk->kill(k_book, vlad()).has_value());
    restart();
    const auto book = desk->book_view("7");
    EXPECT_EQ(book->state, BookState::Halted);
    EXPECT_EQ(book->own.by, "vlad");
    EXPECT_EQ(desk->view().capital, d("150000"));
    EXPECT_EQ(effective(Param::OrderNotional), d("3000"));
    const auto verdict = ask(buy(k_start));
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error().why, RiskReject::BookHalted);

    ASSERT_TRUE(desk->set_state(k_book, BookState::Reducing, vlad(true)).has_value());
    restart();
    EXPECT_EQ(desk->book_view("7")->state, BookState::Reducing);
}

TEST_F(Desk, AHaltThatCannotBeSavedStillHaltsAndSaysSo) {
    capital();
    kernel.open[k_book] = 1;
    store.fail = true;
    const auto killed = desk->kill(k_book, vlad());
    ASSERT_FALSE(killed.has_value());
    EXPECT_EQ(killed.error().error, DeskError::NotDurable);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);
    EXPECT_EQ(kernel.cancels.size(), 1U);
}

TEST_F(Desk, RepeatedExecutionHaltsTheBookUntilAPersonReEnablesIt) {
    kernel.per_book[k_book] = {.max_repeat_entries = 2, .repeat_window_ns = 60 * k_s};
    capital();
    kernel.open[k_book] = 1;
    const auto instrument = buy(k_start).instrument;
    std::uint64_t at = k_start;
    for (int i = 0; i < 2; ++i) {
        desk->filled(k_book, instrument);
        ASSERT_TRUE(ask(buy(at += k_s)).has_value());
    }
    desk->filled(k_book, instrument);
    const auto tripped = ask(buy(at += k_s));
    ASSERT_FALSE(tripped.has_value());
    EXPECT_EQ(tripped.error().why, RiskReject::RepeatedExecution);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);
    EXPECT_EQ(desk->book_view("7")->own.by, "strategyd");
    EXPECT_EQ(audit.rows.back().action, "risk.book.tripped");
    EXPECT_EQ(kernel.cancels.size(), 1U);
    EXPECT_EQ(notices.count("book_halted"), 1U);

    // a window later it is still off
    clock.advance(600 * k_s);
    EXPECT_EQ(ask(buy(at + 600 * k_s)).error().why, RiskReject::BookHalted);
    restart();
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);

    ASSERT_TRUE(desk->set_state(k_book, BookState::Active, vlad(true)).has_value());
    EXPECT_TRUE(ask(buy(at + 601 * k_s)).has_value());
}

TEST_F(Desk, ResumeForgivesTheCyclesThatStoppedTheBook) {
    kernel.per_book[k_book] = {.max_repeat_entries = 1, .repeat_window_ns = 60 * k_s};
    capital();
    const auto instrument = buy(k_start).instrument;
    desk->filled(k_book, instrument);
    ASSERT_TRUE(ask(buy(k_start)).has_value());
    desk->filled(k_book, instrument);
    ASSERT_FALSE(ask(buy(k_start + k_s)).has_value());
    ASSERT_TRUE(desk->set_state(k_book, BookState::Active, vlad(true)).has_value());
    desk->filled(k_book, instrument);
    EXPECT_TRUE(ask(buy(k_start + 2 * k_s)).has_value()) << "it would otherwise trip again on the next fill";
}

TEST_F(Desk, TheKillKeepsPullingWhileOrdersRemainAndStopsWhenTheyAreGone) {
    capital();
    kernel.open[k_book] = 2;
    ASSERT_TRUE(desk->kill(k_book, vlad()).has_value());
    ASSERT_EQ(kernel.cancels.size(), 1U);
    seconds(6);
    EXPECT_EQ(kernel.cancels.size(), 2U);
    kernel.open[k_book] = 0;
    seconds(30);
    EXPECT_EQ(kernel.cancels.size(), 2U);
}

TEST_F(Desk, LossSpeedComesFromRealisedPlusUnrealisedOverEachWindow) {
    capital();
    seconds(10);
    kernel.pnls[k_book] = {.realized = d("-100"), .unrealized = d("-50"), .positions = 1};
    seconds(5);
    const auto pnl = desk->book_view("7")->pnl;
    EXPECT_EQ(pnl.total, d("-150"));
    EXPECT_EQ(pnl.m1.change, d("-150"));
    EXPECT_EQ(pnl.h1.change, d("-150"));
    seconds(120);
    const auto later = desk->book_view("7")->pnl;
    EXPECT_EQ(later.m1.change, d("0")) << "the loss is outside the last minute";
    EXPECT_EQ(later.m5.change, d("-150"));
    EXPECT_EQ(later.h1.change, d("-150"));
    EXPECT_EQ(later.day, d("-150"));
}

TEST_F(Desk, CrossingALevelNotifiesOnceAndTakesNoAction) {
    capital();
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::Loss1m, LimitMode::Percent, "1"), vlad()).has_value());
    seconds(5);
    kernel.pnls[k_book] = {.realized = d("-999")};
    seconds(2);
    EXPECT_EQ(notices.count("loss_speed"), 0U);
    kernel.pnls[k_book] = {.realized = d("-1000")};
    seconds(5);
    EXPECT_EQ(notices.count("loss_speed"), 1U);
    EXPECT_TRUE(desk->book_view("7")->pnl.m1.past_level);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Active);
    EXPECT_TRUE(ask(buy(clock.wall().ns)).has_value());
    // out of the window it re-arms
    seconds(70);
    EXPECT_FALSE(desk->book_view("7")->pnl.m1.past_level);
    kernel.pnls[k_book] = {.realized = d("-2500")};
    seconds(2);
    EXPECT_EQ(notices.count("loss_speed"), 2U);
}

TEST_F(Desk, TheLogGetsASteadyLineAtEachCadence) {
    capital();
    kernel.pnls[k_book] = {.realized = d("5"), .positions = 1};
    seconds(181);
    const auto steady = notices.count("pnl");
    EXPECT_GE(steady, 3U);
    EXPECT_LE(steady, 4U);
    EXPECT_EQ(notices.rows.front().level, NoticeLevel::Steady);
}

TEST_F(Desk, ABookWithAnUnmarkedPositionIsNotSampled) {
    capital();
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::Loss1m, LimitMode::Absolute, "10"), vlad()).has_value());
    kernel.pnls[k_book] = {.realized = d("100"), .unrealized = d("50"), .positions = 1};
    seconds(5);
    // the mark goes: the position drops out of the sum, which must not read as a loss
    kernel.pnls[k_book] = {.realized = d("100"), .positions = 1, .unmarked = 1};
    seconds(5);
    EXPECT_EQ(notices.count("loss_speed"), 0U);
    EXPECT_EQ(desk->book_view("7")->pnl.unmarked, 1U);
}

TEST_F(Desk, ADailyLossTakesNoActionByDefault) {
    capital();
    ASSERT_TRUE(desk->set_limits(k_book, one(Param::DailyLoss, LimitMode::Absolute, "500"), vlad()).has_value());
    seconds(2);
    kernel.pnls[k_book] = {.realized = d("-600")};
    seconds(2);
    EXPECT_EQ(notices.count("daily_loss"), 1U);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Active);
}

TEST_F(Desk, ADailyLossActionMovesTheStateAndNeverComesBackOnItsOwn) {
    capital();
    auto patch = one(Param::DailyLoss, LimitMode::Absolute, "500");
    patch.loss_action = LossAction::Reducing;
    ASSERT_TRUE(desk->set_limits(k_book, patch, vlad()).has_value());
    seconds(2);
    kernel.pnls[k_book] = {.realized = d("-600")};
    seconds(2);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Reducing);
    EXPECT_EQ(audit.rows.back().action, "risk.book.loss_action");
    kernel.pnls[k_book] = {.realized = d("100")};
    seconds(2);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Reducing);

    ASSERT_TRUE(desk->set_state(k_book, BookState::Active, vlad(true)).has_value());
    patch.loss_action = LossAction::Halted;
    ASSERT_TRUE(desk->set_limits(k_book, patch, vlad()).has_value());
    kernel.open[k_book] = 1;
    kernel.pnls[k_book] = {.realized = d("-700")};
    seconds(2);
    EXPECT_EQ(desk->book_view("7")->state, BookState::Halted);
    EXPECT_FALSE(kernel.cancels.empty());
}

TEST_F(Desk, AHaltedBookPullsWhatStillRestsAfterARestart) {
    capital();
    kernel.open[k_book] = 2;
    ASSERT_TRUE(desk->kill(k_book, vlad()).has_value());
    const auto before = kernel.cancels.size();
    restart();
    seconds(1);
    EXPECT_EQ(kernel.cancels.size(), before + 1) << "nothing else would cancel what a restart left resting";
}

TEST_F(Desk, TheDaysLossSurvivesARestart) {
    capital();
    kernel.pnls[k_book] = {.realized = d("1000")};
    seconds(2);
    kernel.pnls[k_book] = {.realized = d("400")};
    seconds(2);
    restart();
    seconds(2);
    EXPECT_EQ(desk->book_view("7")->pnl.day, d("-600"));
}

TEST_F(Desk, CapitalAboveWhatBalancesReportWarnsAndDoesNotRefuse) {
    kernel.held = {.dollars = d("90000"), .accounts = 2};
    capital("100000");
    const auto view = desk->view();
    EXPECT_NE(view.capital_warning.find("declared capital 100000 is above the 90000 balances report"),
              std::string::npos)
        << view.capital_warning;
    EXPECT_EQ(notices.count("capital_above_balances"), 1U);
    EXPECT_TRUE(ask(buy(k_start)).has_value());

    ASSERT_TRUE(desk->set_capital(d("80000"), vlad()).has_value());
    EXPECT_TRUE(desk->view().capital_warning.empty());
}

TEST_F(Desk, NoWordFromBalancesIsNoWarning) {
    capital("100000");
    EXPECT_TRUE(desk->view().capital_warning.empty());
}

TEST_F(Desk, SeededCapitalNeverReplacesAPersonsFigure) {
    desk->seed_capital(d("1000000"), "cli");
    EXPECT_EQ(desk->view().capital, d("1000000"));
    EXPECT_EQ(audit.rows.back().action, "risk.capital.seed");
    ASSERT_TRUE(desk->set_capital(d("5000"), vlad()).has_value());
    desk->seed_capital(d("1000000"), "cli");
    EXPECT_EQ(desk->view().capital, d("5000"));
}

TEST_F(Desk, AStoreThatCannotBeReadRefusesToLoad) {
    class Broken final : public SettingsStore {
    public:
        [[nodiscard]] std::expected<std::optional<DeskSettings>, std::string> load() override {
            return std::unexpected{std::string{"torn"}};
        }

        [[nodiscard]] std::expected<void, std::string> save(const DeskSettings&) override { return {}; }
    } broken;

    RiskDesk fresh{kernel, broken, audit, notices, clock};
    EXPECT_FALSE(fresh.load().has_value());
}

} // namespace
} // namespace raikiri::bookrisk
