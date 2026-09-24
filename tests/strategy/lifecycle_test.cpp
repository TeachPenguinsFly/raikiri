#include <raikiri/strategy/lifecycle.hpp>

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>

namespace raikiri::strategy {
namespace {

TEST(StrategyLifecycle, EverySquareOfTheTableIsStated) {
    EXPECT_EQ(step(Phase::Created, Transition::Start).value(), Phase::Running);
    EXPECT_EQ(step(Phase::Created, Transition::Stop).error(), LifecycleError::NotRunning);
    EXPECT_EQ(step(Phase::Running, Transition::Start).error(), LifecycleError::AlreadyRunning);
    EXPECT_EQ(step(Phase::Running, Transition::Stop).value(), Phase::Stopped);
    EXPECT_EQ(step(Phase::Stopped, Transition::Start).error(), LifecycleError::Finished);
    EXPECT_EQ(step(Phase::Stopped, Transition::Stop).error(), LifecycleError::Finished);
}

TEST(StrategyLifecycle, TheOnlyPathIsCreatedToRunningToStopped) {
    auto phase = Phase::Created;
    phase = step(phase, Transition::Start).value();
    EXPECT_EQ(phase, Phase::Running);
    phase = step(phase, Transition::Stop).value();
    EXPECT_EQ(phase, Phase::Stopped);
    EXPECT_FALSE(step(phase, Transition::Start).has_value());
    EXPECT_FALSE(step(phase, Transition::Stop).has_value());
}

TEST(StrategyLifecycle, StoppingSomethingThatNeverStartedIsRefused) {
    const auto refused = step(Phase::Created, Transition::Stop);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), LifecycleError::NotRunning);
}

TEST(StrategyLifecycle, ARestartIsNeverAnUndoOfAStop) {
    EXPECT_EQ(step(Phase::Stopped, Transition::Start).error(), LifecycleError::Finished);
    EXPECT_NE(step(Phase::Stopped, Transition::Start).error(), LifecycleError::AlreadyRunning);
}

TEST(StrategyLifecycle, TheTableIsConstexprAndTotal) {
    static_assert(step(Phase::Created, Transition::Start).has_value());
    static_assert(!step(Phase::Created, Transition::Stop).has_value());
    static_assert(!step(Phase::Running, Transition::Start).has_value());
    static_assert(step(Phase::Running, Transition::Stop).has_value());
    static_assert(!step(Phase::Stopped, Transition::Start).has_value());
    static_assert(!step(Phase::Stopped, Transition::Stop).has_value());
    SUCCEED();
}

TEST(StrategyLifecycle, EveryNameIsDistinctAndNoneIsTheFallback) {
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
