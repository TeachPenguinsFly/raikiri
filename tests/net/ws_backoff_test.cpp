#include <raikiri/net/ws_backoff.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace raikiri::net {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;

// a clock the test moves by hand; every call takes its reading
struct FakeClock {
    std::uint64_t now{1'000 * k_s};

    void advance(std::uint64_t ns) noexcept { now += ns; }
};

// fails one handshake the moment the host admits it and returns the delay that engaged
std::uint64_t fail_next(HostBackoff& host, FakeClock& clock, WsFailure kind, std::uint64_t retry_after_ns = 0) {
    if (const auto wait = host.admit(clock.now); wait != 0) {
        clock.advance(wait);
    }
    EXPECT_EQ(host.admit(clock.now), 0U);
    return host.failed(clock.now, kind, retry_after_ns);
}

void expect_near_base(std::uint64_t delay, std::uint64_t base) {
    EXPECT_GE(delay, base - base / 4) << "base " << base;
    EXPECT_LE(delay, base) << "base " << base;
}

TEST(HostBackoff, TransportFailuresGrowFromOneSecondToTheThirtySecondCap) {
    HostBackoff host{};
    FakeClock clock;
    const std::vector<std::uint64_t> bases{1, 2, 4, 8, 16, 30, 30, 30};
    for (const auto base : bases) {
        expect_near_base(fail_next(host, clock, WsFailure::Transport), base * k_s);
    }
    EXPECT_EQ(host.stats().backoffs, bases.size());
}

TEST(HostBackoff, ARefusedHandshakeStartsAtThirtySecondsAndCapsAtTenMinutes) {
    HostBackoff host{};
    FakeClock clock;
    const std::vector<std::uint64_t> bases{30, 60, 120, 240, 480, 600, 600};
    for (const auto base : bases) {
        expect_near_base(fail_next(host, clock, WsFailure::Refused), base * k_s);
    }
    EXPECT_EQ(host.stats().refusals, bases.size());
}

TEST(HostBackoff, NothingIsAdmittedBeforeTheDelayRunsOut) {
    HostBackoff host{};
    FakeClock clock;
    const auto delay = fail_next(host, clock, WsFailure::Refused);
    EXPECT_EQ(host.admit(clock.now), delay);
    clock.advance(delay - 1);
    EXPECT_EQ(host.admit(clock.now), 1U);
    clock.advance(1);
    EXPECT_EQ(host.admit(clock.now), 0U);
}

TEST(HostBackoff, RetryAfterIsHonouredAboveTheFloor) {
    HostBackoff host{};
    FakeClock clock;
    EXPECT_EQ(fail_next(host, clock, WsFailure::Refused, 300 * k_s), 300 * k_s);
    clock.advance(299 * k_s);
    EXPECT_NE(host.admit(clock.now), 0U);
    clock.advance(k_s);
    EXPECT_EQ(host.admit(clock.now), 0U);
}

TEST(HostBackoff, RetryAfterBelowTheFloorDoesNotShortenIt) {
    HostBackoff host{};
    FakeClock clock;
    expect_near_base(fail_next(host, clock, WsFailure::Refused, 2 * k_s), 30 * k_s);
}

TEST(HostBackoff, AHealthySocketResetsTheStreakToTheFloor) {
    HostBackoff host{};
    FakeClock clock;
    for (int i = 0; i < 4; ++i) {
        (void)fail_next(host, clock, WsFailure::Refused);
    }
    host.healthy();
    EXPECT_EQ(host.stats().streak, 0U);
    expect_near_base(fail_next(host, clock, WsFailure::Transport), 1 * k_s);
}

TEST(HostBackoff, AnOpenAloneDoesNotResetTheStreak) {
    HostBackoff host{};
    FakeClock clock;
    (void)fail_next(host, clock, WsFailure::Transport);
    (void)fail_next(host, clock, WsFailure::Transport);
    clock.advance(10 * k_s);
    ASSERT_EQ(host.admit(clock.now), 0U);
    host.opened();
    clock.advance(k_s);
    expect_near_base(host.failed(clock.now, WsFailure::Transport), 4 * k_s);
}

TEST(HostBackoff, SocketsFailingTogetherEngageOneBackoff) {
    HostBackoff host{};
    FakeClock clock;
    for (int socket = 0; socket < 12; ++socket) {
        ASSERT_EQ(host.admit(clock.now), 0U);
    }
    const auto first = host.failed(clock.now, WsFailure::Transport);
    expect_near_base(first, 1 * k_s);
    for (int socket = 1; socket < 12; ++socket) {
        EXPECT_EQ(host.failed(clock.now, WsFailure::Transport), 0U);
    }
    EXPECT_EQ(host.stats().streak, 1U);
    EXPECT_EQ(host.stats().backoffs, 1U);
}

TEST(HostBackoff, WhileFailingTheHostTakesOneHandshakeUntilOneOpens) {
    HostBackoff host{};
    FakeClock clock;
    clock.advance(fail_next(host, clock, WsFailure::Refused));
    ASSERT_EQ(host.admit(clock.now), 0U);
    for (int socket = 1; socket < 12; ++socket) {
        EXPECT_NE(host.admit(clock.now), 0U);
    }
    host.opened();
    for (int socket = 1; socket < 12; ++socket) {
        EXPECT_EQ(host.admit(clock.now), 0U);
    }
}

TEST(HostBackoff, AProbeThatNeverAnswersFreesTheSlot) {
    HostBackoff host{};
    FakeClock clock;
    clock.advance(fail_next(host, clock, WsFailure::Transport));
    ASSERT_EQ(host.admit(clock.now), 0U);
    clock.advance(host.policy().probe_timeout_ns);
    EXPECT_EQ(host.admit(clock.now), 0U);
}

TEST(HostBackoff, TheHostCapHoldsAcrossSocketsWhateverTheirBackoffAllows) {
    HostBackoff host{WsBackoffPolicy{.attempts = 5, .attempts_window_ns = 60 * k_s}};
    FakeClock clock;
    std::size_t admitted = 0;
    for (int round = 0; round < 6; ++round) {
        for (int socket = 0; socket < 12; ++socket) {
            admitted += host.admit(clock.now) == 0 ? 1U : 0U;
        }
        clock.advance(5 * k_s);
    }
    EXPECT_EQ(admitted, 5U);
    clock.advance(60 * k_s);
    EXPECT_EQ(host.admit(clock.now), 0U);
}

TEST(HostBackoff, ARefusalDuringATransportBackoffMovesToTheLongFloor) {
    HostBackoff host{};
    FakeClock clock;
    ASSERT_EQ(host.admit(clock.now), 0U);
    ASSERT_EQ(host.admit(clock.now), 0U);
    expect_near_base(host.failed(clock.now, WsFailure::Transport), 1 * k_s);
    expect_near_base(host.failed(clock.now, WsFailure::Refused), 30 * k_s);
}

TEST(HostBackoff, ATransportFailureAfterRefusalsStaysOnTheLongLadder) {
    HostBackoff host{};
    FakeClock clock;
    (void)fail_next(host, clock, WsFailure::Refused);
    expect_near_base(fail_next(host, clock, WsFailure::Transport), 60 * k_s);
}

TEST(HostBackoff, SocketsToOneHostAndPortShareOneBudget) {
    const auto first = host_backoff("ws.example.test", "443", {});
    const auto second = host_backoff("ws.example.test", "443", {});
    const auto other_port = host_backoff("ws.example.test", "8443", {});
    EXPECT_EQ(first.get(), second.get());
    EXPECT_NE(first.get(), other_port.get());
}

TEST(HostBackoff, RetryAfterIsReadInSecondsAndADateIsIgnored) {
    EXPECT_EQ(retry_after_ns("120"), 120 * k_s);
    EXPECT_EQ(retry_after_ns(""), 0U);
    EXPECT_EQ(retry_after_ns("Wed, 21 Oct 2026 07:28:00 GMT"), 0U);
    EXPECT_EQ(retry_after_ns("-5"), 0U);
}

} // namespace
} // namespace raikiri::net
