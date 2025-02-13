#include <raikiri/domain/order_events.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri {
namespace {

using S = OrderStatus;
using E = OrderEventType;

constexpr std::array<S, 12> k_all_statuses{
    S::Initialized, S::Denied,        S::Submitted,       S::Accepted,
    S::Rejected,    S::Triggered,     S::PendingUpdate,   S::PendingCancel,
    S::PartiallyFilled, S::Canceled,  S::Expired,         S::Filled,
};

constexpr std::array<E, 16> k_all_events{
    E::Initialized, E::Denied,         E::Submitted,     E::Accepted,
    E::Rejected,    E::Triggered,      E::PendingUpdate, E::Updated,
    E::ModifyRejected, E::PendingCancel, E::Canceled,    E::CancelRejected,
    E::Expired,     E::Filled,         E::ReconGap,      E::ReconClosed,
};

TEST(OrderFsm, HappyPathLimitOrder) {
    S s = S::Initialized;
    s = *transition(s, E::Submitted);
    EXPECT_EQ(s, S::Submitted);
    s = *transition(s, E::Accepted);
    EXPECT_EQ(s, S::Accepted);
    s = *transition(s, E::Filled, /*full_fill=*/false);
    EXPECT_EQ(s, S::PartiallyFilled);
    s = *transition(s, E::Filled, /*full_fill=*/true);
    EXPECT_EQ(s, S::Filled);
}

TEST(OrderFsm, FastFillBeatsAck) {
    // bybit/binance user streams regularly deliver the fill before the REST
    // ack returns.
    EXPECT_EQ(*transition(S::Submitted, E::Filled, true), S::Filled);
    EXPECT_EQ(*transition(S::Submitted, E::Filled, false), S::PartiallyFilled);
}

TEST(OrderFsm, RiskDenialIsTerminal) {
    EXPECT_EQ(*transition(S::Initialized, E::Denied), S::Denied);
    for (const auto e : k_all_events) {
        if (e == E::ReconGap) {
            continue;
        }
        EXPECT_FALSE(transition(S::Denied, e).has_value()) << to_string(e);
    }
}

TEST(OrderFsm, CancelFillRace) {
    // Cancel confirmed, then the fill that matched first arrives: fills
    // outrank cancels — money moved.
    EXPECT_EQ(*transition(S::Canceled, E::Filled, true), S::Filled);
    // Partial fill after cancel: quantities update, status stays Canceled.
    EXPECT_EQ(*transition(S::Canceled, E::Filled, false), S::Canceled);
}

TEST(OrderFsm, PendingCancelResolution) {
    EXPECT_EQ(*transition(S::PendingCancel, E::Canceled), S::Canceled);
    // Too-late cancel returns the order to its remembered working status.
    EXPECT_EQ(*transition(S::PendingCancel, E::CancelRejected, false, S::PartiallyFilled),
              S::PartiallyFilled);
    // Full fill wins the race outright; partial keeps the cancel pending.
    EXPECT_EQ(*transition(S::PendingCancel, E::Filled, true), S::Filled);
    EXPECT_EQ(*transition(S::PendingCancel, E::Filled, false), S::PendingCancel);
}

TEST(OrderFsm, ModifyResolution) {
    EXPECT_EQ(*transition(S::Accepted, E::PendingUpdate), S::PendingUpdate);
    EXPECT_EQ(*transition(S::PendingUpdate, E::Updated, false, S::Accepted), S::Accepted);
    EXPECT_EQ(*transition(S::PendingUpdate, E::ModifyRejected, false, S::PartiallyFilled),
              S::PartiallyFilled);
    // Fill lands while the amend is in flight.
    EXPECT_EQ(*transition(S::PendingUpdate, E::Filled, true), S::Filled);
}

TEST(OrderFsm, SyntheticRestReject) {
    // A REST error on submit synthesizes Rejected: venues do not WS-notify
    // REST-level failures.
    EXPECT_EQ(*transition(S::Submitted, E::Rejected), S::Rejected);
    // Late reject after ack (margin check second).
    EXPECT_EQ(*transition(S::Accepted, E::Rejected), S::Rejected);
}

TEST(OrderFsm, ReconGapIsStatusNeutralEverywhere) {
    for (const auto s : k_all_statuses) {
        const auto next = transition(s, E::ReconGap);
        ASSERT_TRUE(next.has_value()) << to_string(s);
        EXPECT_EQ(*next, s);
    }
}

TEST(OrderFsm, ReconClosedClosesEveryWorkingStatus) {
    for (const auto s : k_all_statuses) {
        if (is_terminal(s)) {
            continue;
        }
        const auto next = transition(s, E::ReconClosed);
        ASSERT_TRUE(next.has_value()) << to_string(s);
        EXPECT_EQ(*next, S::Canceled);
    }
}

// Property: no event sequence can leave a hard-terminal status, except the
// documented Canceled/Expired -> Filled races.
TEST(OrderFsmProperty, TerminalStatusesHaveNoExits) {
    for (const auto s : {S::Denied, S::Rejected, S::Filled}) {
        for (const auto e : k_all_events) {
            if (e == E::ReconGap) {
                continue;
            }
            EXPECT_FALSE(transition(s, e, true).has_value())
                << to_string(s) << " + " << to_string(e);
            EXPECT_FALSE(transition(s, e, false).has_value())
                << to_string(s) << " + " << to_string(e);
        }
    }
}

// Property: every reachable non-terminal status has a path to a terminal
// status (no traps).
TEST(OrderFsmProperty, NoTrapStates) {
    for (const auto s : k_all_statuses) {
        if (is_terminal(s)) {
            continue;
        }
        bool has_terminal_exit = false;
        for (const auto e : k_all_events) {
            for (const bool full : {true, false}) {
                const auto next = transition(s, e, full);
                if (next.has_value() && is_terminal(*next)) {
                    has_terminal_exit = true;
                }
            }
        }
        EXPECT_TRUE(has_terminal_exit) << to_string(s);
    }
}

// Property: the table is total over its own domain — every (status, event,
// fill) triple either transitions or reports TransitionError carrying the
// exact pair, never crashes.
TEST(OrderFsmProperty, TotalFunction) {
    for (const auto s : k_all_statuses) {
        for (const auto e : k_all_events) {
            for (const bool full : {true, false}) {
                const auto next = transition(s, e, full);
                if (!next.has_value()) {
                    EXPECT_EQ(next.error().from, s);
                    EXPECT_EQ(next.error().event, e);
                }
            }
        }
    }
}

} // namespace
} // namespace raikiri
