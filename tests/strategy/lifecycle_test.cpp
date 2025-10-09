// The transition table and every refusal it produces, exhaustively.
//
// A lifecycle whose illegal moves are discovered in production is the thing
// this replaces, so the table is enumerated rather than sampled: three phases
// times two transitions is six cells, and all six are asserted below.

#include <raikiri/strategy/lifecycle.hpp>

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>

namespace raikiri::strategy {
namespace {

TEST(StrategyLifecycle, EverySquareOfTheTableIsStated) {
    // Created
    EXPECT_EQ(step(Phase::Created, Transition::Start).value(), Phase::Running);
    EXPECT_EQ(step(Phase::Created, Transition::Stop).error(), LifecycleError::NotRunning);
    // Running
    EXPECT_EQ(step(Phase::Running, Transition::Start).error(), LifecycleError::AlreadyRunning);
    EXPECT_EQ(step(Phase::Running, Transition::Stop).value(), Phase::Stopped);
    // Stopped — terminal in BOTH directions, and the same reason for both.
    EXPECT_EQ(step(Phase::Stopped, Transition::Start).error(), LifecycleError::Finished);
    EXPECT_EQ(step(Phase::Stopped, Transition::Stop).error(), LifecycleError::Finished);
}

TEST(StrategyLifecycle, TheOnlyPathIsCreatedToRunningToStopped) {
    // Walked rather than asserted cell by cell: the property is that there is
    // ONE route through the table and it does not loop.
    auto phase = Phase::Created;
    phase = step(phase, Transition::Start).value();
    EXPECT_EQ(phase, Phase::Running);
    phase = step(phase, Transition::Stop).value();
    EXPECT_EQ(phase, Phase::Stopped);
    // And from the end, nothing.
    EXPECT_FALSE(step(phase, Transition::Start).has_value());
    EXPECT_FALSE(step(phase, Transition::Stop).has_value());
}

TEST(StrategyLifecycle, StoppingSomethingThatNeverStartedIsRefused) {
    // A stop is the unwind of a start. Waving this through would run on_stop
    // on a strategy whose on_start never ran — a teardown of arrangements it
    // never made.
    const auto refused = step(Phase::Created, Transition::Stop);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), LifecycleError::NotRunning);
}

TEST(StrategyLifecycle, ARestartIsNeverAnUndoOfAStop) {
    // Stopped is terminal on purpose: whatever the strategy remembers
    // describes a market it stopped watching. The refusal names that rather
    // than saying "already running".
    EXPECT_EQ(step(Phase::Stopped, Transition::Start).error(), LifecycleError::Finished);
    EXPECT_NE(step(Phase::Stopped, Transition::Start).error(), LifecycleError::AlreadyRunning);
}

TEST(StrategyLifecycle, TheTableIsConstexprAndTotal) {
    // Total: no input leaves it without an answer, and the compiler proves it
    // by evaluating every square at compile time.
    static_assert(step(Phase::Created, Transition::Start).has_value());
    static_assert(!step(Phase::Created, Transition::Stop).has_value());
    static_assert(!step(Phase::Running, Transition::Start).has_value());
    static_assert(step(Phase::Running, Transition::Stop).has_value());
    static_assert(!step(Phase::Stopped, Transition::Start).has_value());
    static_assert(!step(Phase::Stopped, Transition::Stop).has_value());
    SUCCEED();
}

TEST(StrategyLifecycle, EveryNameIsDistinctAndNoneIsTheFallback) {
    // A screen that says "?" for a phase sends someone to read source.
    const std::array phases{Phase::Created, Phase::Running, Phase::Stopped};
    std::set<std::string> seen;
    for (const auto phase : phases) {
        EXPECT_NE(to_string(phase), "?");
        seen.emplace(to_string(phase));
    }
    EXPECT_EQ(seen.size(), phases.size());

    const std::array errors{
        LifecycleError::AlreadyRunning, LifecycleError::NotRunning, LifecycleError::Finished};
    std::set<std::string> reasons;
    for (const auto error : errors) {
        EXPECT_NE(to_string(error), "?");
        reasons.emplace(to_string(error));
    }
    EXPECT_EQ(reasons.size(), errors.size());
}

} // namespace
} // namespace raikiri::strategy
