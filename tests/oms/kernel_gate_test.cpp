#include <raikiri/oms/kernel_gate.hpp>

#include <gtest/gtest.h>

#include <array>

namespace raikiri::oms {
namespace {

using enum KernelState;
using enum KernelEvent;

constexpr std::array k_states{Running, Degraded, Halted};
constexpr std::array k_events{ExecOverrun, GatewayLost, ReconAgreed, ReconFailed, OperatorResume};

TEST(KernelGate, CancelSurvivesEveryState) {
    // THE ONE INVARIANT THE WHOLE TYPE EXISTS TO PROTECT. A kernel that has
    // lost confidence must still be able to reduce what is already outstanding;
    // a state that forbade cancel would trap risk it no longer understands.
    for (const auto state : k_states) {
        EXPECT_TRUE(verdict_of(state).may_cancel) << to_string(state);
    }
}

TEST(KernelGate, OnlyRunningMayAddRisk) {
    EXPECT_TRUE(verdict_of(Running).may_open);
    EXPECT_FALSE(verdict_of(Degraded).may_open);
    EXPECT_FALSE(verdict_of(Halted).may_open);
}

TEST(KernelGate, NoEventEverMakesTheKernelMoreTrustingByAccident) {
    // Exhaustive over the whole table: 3 states x 2 flags x 5 events. The only
    // transitions that may INCREASE trust are the two written to do so —
    // reconciliation agreeing out of Degraded, and an operator resuming a halt
    // that has already been checked. Anything else that promoted would be a
    // kernel quietly deciding it is fine again.
    const auto trust = [](KernelState s) {
        return s == Running ? 2 : (s == Degraded ? 1 : 0);
    };
    for (const auto state : k_states) {
        for (const bool agreed : {false, true}) {
            for (const auto event : k_events) {
                const KernelStep from{.state = state, .recon_agreed = agreed};
                const auto to = step(from, event);
                if (trust(to.state) <= trust(from.state)) {
                    continue;
                }
                const bool cleared = from.state == Degraded && event == ReconAgreed;
                const bool resumed = from.state == Halted && agreed && event == OperatorResume;
                EXPECT_TRUE(cleared || resumed)
                    << to_string(state) << " agreed=" << agreed << " + " << to_string(event)
                    << " -> " << to_string(to.state);
            }
        }
    }
}

TEST(KernelGate, AnOverrunStopsNewOrdersAndIsClearedByAgreement) {
    // The case the whole type was written for: strategyd counted a dropped
    // execution record and carried on, and a dropped record may be a fill.
    KernelGate gate;
    ASSERT_TRUE(gate.may_open());

    EXPECT_TRUE(gate.on(ExecOverrun, UnixNanos{10}));
    EXPECT_EQ(gate.state(), Degraded);
    EXPECT_FALSE(gate.may_open());
    EXPECT_TRUE(gate.verdict().may_cancel) << "the escape hatch stays open";
    EXPECT_EQ(gate.since().ns, 10U);

    // A second overrun is not a further fall, and must not restamp the clock:
    // the answer to "since when" is the first one.
    EXPECT_FALSE(gate.on(ExecOverrun, UnixNanos{20}));
    EXPECT_EQ(gate.since().ns, 10U);

    EXPECT_TRUE(gate.on(ReconAgreed, UnixNanos{30}));
    EXPECT_EQ(gate.state(), Running);
    EXPECT_TRUE(gate.may_open());
}

TEST(KernelGate, AConfirmedBreakHaltsAndAgreementAloneDoesNotUndoIt) {
    KernelGate gate;
    ASSERT_TRUE(gate.on(ReconFailed, UnixNanos{5}));
    EXPECT_EQ(gate.state(), Halted);

    // The distinction between the two stopped states, and the reason there are
    // two: Degraded clears itself, Halted does not.
    EXPECT_FALSE(gate.on(ReconAgreed, UnixNanos{6}));
    EXPECT_EQ(gate.state(), Halted);
    EXPECT_TRUE(gate.resumable()) << "checked, and now waiting on a person";
}

TEST(KernelGate, AnOperatorCannotResumeABookNobodyHasChecked) {
    // Resuming an unverified book is the original defect with a signature on
    // it. The order is fixed: the venue agrees first, the person second.
    KernelGate gate;
    ASSERT_TRUE(gate.on(ReconFailed, UnixNanos{1}));
    EXPECT_FALSE(gate.resumable());

    EXPECT_FALSE(gate.on(OperatorResume, UnixNanos{2})) << "refused, not obeyed";
    EXPECT_EQ(gate.state(), Halted);

    ASSERT_FALSE(gate.on(ReconAgreed, UnixNanos{3}));
    ASSERT_TRUE(gate.resumable());
    EXPECT_TRUE(gate.on(OperatorResume, UnixNanos{4}));
    EXPECT_EQ(gate.state(), Running);
    EXPECT_TRUE(gate.may_open());
}

TEST(KernelGate, ALostGatewayHaltsFromAnywhereAndForgetsAnyPriorAgreement) {
    // Everything read from a segment belongs to the generation that wrote it.
    // An agreement recorded before the owner left describes a world that is
    // gone, so it must not survive to satisfy the resume check.
    for (const auto state : k_states) {
        const auto to = step({.state = state, .recon_agreed = true}, GatewayLost);
        EXPECT_EQ(to.state, Halted) << to_string(state);
        EXPECT_FALSE(to.recon_agreed) << to_string(state);
    }
}

TEST(KernelGate, TheReasonAndTheInstantSurviveForAScreenToShow) {
    // A screen that says "halted" and cannot say what halted it sends someone
    // to read logs, which is the failure this type is meant to end.
    KernelGate gate;
    gate.on(ExecOverrun, UnixNanos{100});
    EXPECT_EQ(gate.because(), ExecOverrun);
    EXPECT_EQ(gate.since().ns, 100U);

    gate.on(ReconFailed, UnixNanos{200});
    EXPECT_EQ(gate.because(), ReconFailed);
    EXPECT_EQ(gate.since().ns, 200U);
    EXPECT_FALSE(to_string(gate.because()).empty());
}

TEST(KernelGate, EveryStateAndEventNamesItself) {
    for (const auto state : k_states) {
        EXPECT_NE(to_string(state), "?");
    }
    for (const auto event : k_events) {
        EXPECT_NE(to_string(event), "?");
    }
}

} // namespace
} // namespace raikiri::oms
