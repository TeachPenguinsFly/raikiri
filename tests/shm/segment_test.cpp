#include <raikiri/shm/segment.hpp>
#include <raikiri/shm/spsc_ring.hpp>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <string>
#include <vector>

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

    auto client = SegmentClient::attach(path);
    ASSERT_TRUE(client.has_value());
    EXPECT_EQ(client->generation(), 111U);

    auto client_ring = client->open_channel<CmdRing>("cmd_create");
    ASSERT_TRUE(client_ring.has_value());

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

        auto early = SegmentClient::attach(path);
        ASSERT_FALSE(early.has_value());
        EXPECT_EQ(early.error(), SegError::NotReady);

        owner->publish();
        auto client = SegmentClient::attach(path);
        ASSERT_TRUE(client.has_value());

        const auto missing = client->open_channel<CmdRing>("nope");
        ASSERT_FALSE(missing.has_value());
        EXPECT_EQ(missing.error(), SegError::NoSuchChannel);

        using WrongRing = SpscRing<std::uint64_t, 128>;
        const auto wrong = client->open_channel<WrongRing>("cmd_create");
        ASSERT_FALSE(wrong.has_value());
        EXPECT_EQ(wrong.error(), SegError::SizeMismatch);
    }
    const auto gone = SegmentClient::attach(path);
    EXPECT_FALSE(gone.has_value());
}

TEST(Segment, ForeignTagRefusedBothWays) {
    const auto path = test_path("tagged");
    constexpr SegmentTag tag{.magic = 0x5052454449435431ULL, .abi = 3};
    auto owner = SegmentOwner::create(path, 1 << 20, 1, 1000, tag);
    ASSERT_TRUE(owner.has_value());
    ASSERT_NE(owner->create_channel<CmdRing>("cmd_create"), nullptr);
    owner->publish();

    const auto as_raikiri = SegmentClient::attach(path);
    ASSERT_FALSE(as_raikiri.has_value());
    EXPECT_EQ(as_raikiri.error(), SegError::BadMagic);

    const auto old_abi = SegmentClient::attach(path, {.magic = tag.magic, .abi = 2});
    ASSERT_FALSE(old_abi.has_value());
    EXPECT_EQ(old_abi.error(), SegError::BadAbi);

    auto client = SegmentClient::attach(path, tag);
    ASSERT_TRUE(client.has_value());
    EXPECT_TRUE(client->open_channel<CmdRing>("cmd_create").has_value());

    const auto rival = SegmentOwner::create(path, 1 << 20, 2, 2000, tag);
    ASSERT_FALSE(rival.has_value());
    EXPECT_EQ(rival.error(), SegError::AlreadyRunning);

    const auto raikiri_path = test_path("untagged");
    auto plain = SegmentOwner::create(raikiri_path, 1 << 20, 1, 1000);
    ASSERT_TRUE(plain.has_value());
    plain->publish();
    const auto foreign = SegmentClient::attach(raikiri_path, tag);
    ASSERT_FALSE(foreign.has_value());
    EXPECT_EQ(foreign.error(), SegError::BadMagic);
    EXPECT_TRUE(SegmentClient::attach(raikiri_path).has_value());
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
        const auto rival = SegmentOwner::create(path, 1 << 20, 2, /*now_ns=*/1000 + k_owner_stale_ns + 1);
        ASSERT_FALSE(rival.has_value()); // alive pid + stale beat = hung, not dead
        EXPECT_EQ(rival.error(), SegError::AlreadyRunning);
    }
    auto fresh = SegmentOwner::create(path, 1 << 20, 3, 5000);
    EXPECT_TRUE(fresh.has_value());
}

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

std::expected<SegmentOwner, SegError> published(const std::string& path) {
    auto owner = SegmentOwner::create(path, 1 << 20, 1, 1000);
    if (owner.has_value()) {
        (void)owner->create_channel<CmdRing>("cmd_create");
        owner->publish();
    }
    return owner;
}

TEST(Segment, AbiMismatchRefused) {
    const auto path = test_path("abi");
    auto owner = SegmentOwner::create(path, 1 << 20, 1, 1000, {.magic = k_magic, .abi = k_abi_version - 1});
    ASSERT_TRUE(owner.has_value());
    owner->publish();
    const auto current = SegmentClient::attach(path);
    ASSERT_FALSE(current.has_value());
    EXPECT_EQ(current.error(), SegError::BadAbi);

    const auto newer_path = test_path("abi-new");
    auto newer = published(newer_path);
    ASSERT_TRUE(newer.has_value());
    const auto older = SegmentClient::attach(newer_path, {.magic = k_magic, .abi = k_abi_version - 1});
    ASSERT_FALSE(older.has_value());
    EXPECT_EQ(older.error(), SegError::BadAbi);
}

TEST(ReaderSlots, ReRegisteringReusesTheSlot) {
    const auto path = test_path("reuse");
    auto owner = published(path);
    ASSERT_TRUE(owner.has_value());

    auto first = SegmentClient::attach(path);
    ASSERT_TRUE(first.has_value());
    const auto slot = first->register_reader(::getpid(), "probe");
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(first->register_reader(::getpid(), "probe"), slot) << "the same client keeps its slot";
    EXPECT_EQ(first->register_reader(::getpid(), "other"), slot) << "a registered client keeps its slot";

    auto second = SegmentClient::attach(path);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->register_reader(::getpid(), "probe"), slot) << "a second attach by the same process shares";

    auto stranger = SegmentClient::attach(path);
    ASSERT_TRUE(stranger.has_value());
    const auto other = stranger->register_reader(::getpid(), "stranger");
    ASSERT_TRUE(other.has_value());
    EXPECT_NE(*other, *slot);

    first->release_reader();
    EXPECT_FALSE(first->reader_listed());
    auto third = SegmentClient::attach(path);
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->register_reader(::getpid(), "probe"), slot) << "still held by the second client";

    { const SegmentClient dropped{std::move(*second)}; }
    third->release_reader();
    stranger->release_reader();
    auto fresh = SegmentClient::attach(path);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh->register_reader(::getpid(), "newcomer"), 0U) << "every slot came back";
}

TEST(ReaderSlots, ThirtyTwoReadersThenUnlistedReading) {
    const auto path = test_path("capacity");
    auto owner = published(path);
    ASSERT_TRUE(owner.has_value());
    static_assert(k_max_readers == 32);

    std::vector<SegmentClient> readers;
    readers.reserve(k_max_readers);
    for (std::size_t i = 0; i < k_max_readers; ++i) {
        auto client = SegmentClient::attach(path);
        ASSERT_TRUE(client.has_value());
        const auto slot = client->register_reader(::getpid(), "r" + std::to_string(i));
        ASSERT_TRUE(slot.has_value()) << i;
        EXPECT_EQ(*slot, i);
        readers.push_back(std::move(*client));
    }

    auto late = SegmentClient::attach(path);
    ASSERT_TRUE(late.has_value());
    const auto none = late->register_reader(::getpid(), "late");
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error(), SegError::NoReaderSlot);
    EXPECT_FALSE(late->reader_listed());
    EXPECT_TRUE(late->open_channel<CmdRing>("cmd_create").has_value()) << "no slot never blocks reading";
    late->beat_reader(k_max_readers, 5);

    readers.pop_back();
    EXPECT_EQ(late->register_reader(::getpid(), "late"), k_max_readers - 1);
}

TEST(ReaderSlots, KilledReaderSlotReclaimed) {
    const auto path = test_path("killed");
    auto owner = published(path);
    ASSERT_TRUE(owner.has_value());

    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(fds[0]);
        auto client = SegmentClient::attach(path);
        const char ok = client.has_value() && client->register_reader(::getpid(), "child").has_value() ? 'y' : 'n';
        if (::write(fds[1], &ok, 1) != 1) {
            ::_exit(3);
        }
        for (;;) {
            ::pause();
        }
    }
    ::close(fds[1]);
    char ok = 0;
    ASSERT_EQ(::read(fds[0], &ok, 1), 1);
    ::close(fds[0]);
    ASSERT_EQ(ok, 'y');

    std::vector<SegmentClient> readers;
    readers.reserve(k_max_readers);
    const auto fill = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            auto client = SegmentClient::attach(path);
            if (!client.has_value() || !client->register_reader(::getpid(), "p" + std::to_string(readers.size()))) {
                return false;
            }
            readers.push_back(std::move(*client));
        }
        return true;
    };
    ASSERT_TRUE(fill(k_max_readers - 1));
    ASSERT_FALSE(fill(1)) << "the live child still holds one slot";

    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(fill(1)) << "a dead pid's slot is taken over";
}

} // namespace
} // namespace raikiri::shm
