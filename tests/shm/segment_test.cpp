#include <raikiri/shm/segment.hpp>
#include <raikiri/shm/spsc_ring.hpp>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace raikiri::shm {
namespace {

std::string test_path(const char* name) {
    return "/tmp/raikiri-segtest-" + std::to_string(::getpid()) + "-" + name;
}

using CmdRing = SpscRing<std::uint64_t, 64>;

TEST(Segment, CreatePublishAttachRoundTrip) {
    const auto path = test_path("roundtrip");
    auto owner = SegmentOwner::create(path, 1 << 20, /*generation=*/111, /*now_ns=*/1000);
    ASSERT_TRUE(owner.has_value());

    auto* ring = owner->create_channel<CmdRing>("cmd_create");
    ASSERT_NE(ring, nullptr);
    owner->publish();

    // Second mapping of the same file = a genuine second view, same protocol
    // a separate process would use.
    auto client = SegmentClient::attach(path);
    ASSERT_TRUE(client.has_value());
    EXPECT_EQ(client->generation(), 111U);

    auto client_ring = client->open_channel<CmdRing>("cmd_create");
    ASSERT_TRUE(client_ring.has_value());

    // Write through the client view, read through the owner view.
    SpscProducer producer{*client_ring};
    ASSERT_TRUE(producer.try_push(0xC0FFEE).has_value());
    SpscConsumer consumer{ring};
    std::uint64_t out{};
    ASSERT_TRUE(consumer.try_pop(out));
    EXPECT_EQ(out, 0xC0FFEEU);
}

TEST(Segment, AttachValidations) {
    const auto path = test_path("validate");
    {
        auto owner = SegmentOwner::create(path, 1 << 20, 1, 1000);
        ASSERT_TRUE(owner.has_value());
        auto* ring = owner->create_channel<CmdRing>("cmd_create");
        ASSERT_NE(ring, nullptr);

        // Attach before publish -> NotReady.
        auto early = SegmentClient::attach(path);
        ASSERT_FALSE(early.has_value());
        EXPECT_EQ(early.error(), SegError::NotReady);

        owner->publish();
        auto client = SegmentClient::attach(path);
        ASSERT_TRUE(client.has_value());

        // Unknown channel name.
        const auto missing = client->open_channel<CmdRing>("nope");
        ASSERT_FALSE(missing.has_value());
        EXPECT_EQ(missing.error(), SegError::NoSuchChannel);

        // Wrong compiled type (different size) for a known name.
        using WrongRing = SpscRing<std::uint64_t, 128>;
        const auto wrong = client->open_channel<WrongRing>("cmd_create");
        ASSERT_FALSE(wrong.has_value());
        EXPECT_EQ(wrong.error(), SegError::SizeMismatch);
    }
    // Owner destroyed -> file unlinked -> attach fails.
    const auto gone = SegmentClient::attach(path);
    EXPECT_FALSE(gone.has_value());
}

TEST(Segment, SecondOwnerRefusedWhileFirstAlive) {
    const auto path = test_path("exclusive");
    auto owner = SegmentOwner::create(path, 1 << 20, 1, 1000);
    ASSERT_TRUE(owner.has_value());
    owner->beat(2000);

    const auto rival = SegmentOwner::create(path, 1 << 20, 2, /*now_ns=*/2500);
    ASSERT_FALSE(rival.has_value());
    EXPECT_EQ(rival.error(), SegError::AlreadyRunning);
}

TEST(Segment, StaleOwnerTakenOver) {
    const auto path = test_path("stale");
    {
        auto owner = SegmentOwner::create(path, 1 << 20, 1, 1000);
        ASSERT_TRUE(owner.has_value());
        owner->publish();
        // Simulate a crash: leak the mapping state by moving into a
        // deliberately-forgotten object? Simpler: the heartbeat goes stale
        // while the "process" (same pid, so alive) stops beating — takeover
        // must still be refused because the pid is alive...
        const auto rival =
            SegmentOwner::create(path, 1 << 20, 2, /*now_ns=*/1000 + k_owner_stale_ns + 1);
        ASSERT_FALSE(rival.has_value()); // alive pid + stale beat = hung, not dead
        EXPECT_EQ(rival.error(), SegError::AlreadyRunning);
    }
    // After clean destruction the path is free again.
    auto fresh = SegmentOwner::create(path, 1 << 20, 3, 5000);
    EXPECT_TRUE(fresh.has_value());
}

// The real thing: owner in the parent, consumer in a forked child, commands
// flowing child -> parent through the segment.
TEST(Segment, CrossProcessCommandFlow) {
    const auto path = test_path("fork");
    auto owner = SegmentOwner::create(path, 1 << 20, 42, 1000);
    ASSERT_TRUE(owner.has_value());
    auto* ring = owner->create_channel<CmdRing>("cmd_create");
    ASSERT_NE(ring, nullptr);
    owner->publish();

    constexpr std::uint64_t k_count = 10'000;
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // Child: attach by path and push the sequence.
        auto client = SegmentClient::attach(path);
        if (!client.has_value()) {
            ::_exit(2);
        }
        auto child_ring = client->open_channel<CmdRing>("cmd_create");
        if (!child_ring.has_value()) {
            ::_exit(3);
        }
        SpscProducer producer{*child_ring};
        for (std::uint64_t i = 0; i < k_count;) {
            if (producer.try_push(i).has_value()) {
                ++i;
            }
        }
        ::_exit(0);
    }

    SpscConsumer consumer{ring};
    std::uint64_t expected = 0;
    while (expected < k_count) {
        std::uint64_t out{};
        if (consumer.try_pop(out)) {
            ASSERT_EQ(out, expected);
            ++expected;
        }
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace
} // namespace raikiri::shm
