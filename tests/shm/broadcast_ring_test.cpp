#include <raikiri/shm/broadcast_ring.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

namespace raikiri::shm {
namespace {

struct Event {
    std::uint64_t seq{0};
    std::uint64_t payload{0};
};

TEST(BroadcastRing, PublishPollInOrder) {
    auto ring = std::make_unique<BroadcastRing<Event, 16>>();
    BroadcastReader reader{ring.get()};

    Event out{};
    EXPECT_EQ(reader.poll(out), PollResult::Empty);

    ring->publish({1, 100});
    ring->publish({2, 200});
    ASSERT_EQ(reader.poll(out), PollResult::Item);
    EXPECT_EQ(out.payload, 100U);
    ASSERT_EQ(reader.poll(out), PollResult::Item);
    EXPECT_EQ(out.payload, 200U);
    EXPECT_EQ(reader.poll(out), PollResult::Empty);
}

TEST(BroadcastRing, IndependentReadersIndependentCursors) {
    auto ring = std::make_unique<BroadcastRing<Event, 16>>();
    BroadcastReader fast{ring.get()};
    BroadcastReader slow{ring.get()};

    ring->publish({1, 10});
    ring->publish({2, 20});

    Event out{};
    ASSERT_EQ(fast.poll(out), PollResult::Item);
    ASSERT_EQ(fast.poll(out), PollResult::Item);
    EXPECT_EQ(fast.poll(out), PollResult::Empty);

    // The slow reader still sees everything from its own cursor.
    ASSERT_EQ(slow.poll(out), PollResult::Item);
    EXPECT_EQ(out.payload, 10U);
}

TEST(BroadcastRing, LateAttachStartsAtHead) {
    auto ring = std::make_unique<BroadcastRing<Event, 16>>();
    ring->publish({1, 10});
    ring->publish({2, 20});

    BroadcastReader late{ring.get()};
    Event out{};
    EXPECT_EQ(late.poll(out), PollResult::Empty); // starts after existing items

    ring->publish({3, 30});
    ASSERT_EQ(late.poll(out), PollResult::Item);
    EXPECT_EQ(out.payload, 30U);
}

TEST(BroadcastRing, OverrunDetectedWithGapCount) {
    auto ring = std::make_unique<BroadcastRing<Event, 8>>();
    BroadcastReader reader{ring.get()};

    // Fill well past capacity while the reader sleeps.
    for (std::uint64_t i = 1; i <= 20; ++i) {
        ring->publish({i, i});
    }
    Event out{};
    ASSERT_EQ(reader.poll(out), PollResult::Overrun);
    EXPECT_EQ(reader.gap(), 20U); // it missed everything
    EXPECT_EQ(reader.poll(out), PollResult::Empty);

    ring->publish({21, 21});
    ASSERT_EQ(reader.poll(out), PollResult::Item);
    EXPECT_EQ(out.payload, 21U); // clean resume after resync
}

// TSan + torn-read target: producer laps the ring while readers poll at full
// speed. Every delivered item must be internally coherent and sequences must
// advance monotonically through overruns.
TEST(BroadcastRingStress, LappedReadersSeeCoherentItemsOnly) {
    auto ring = std::make_unique<BroadcastRing<Event, 64>>();
    constexpr std::uint64_t k_publishes = 500'000;

    std::thread producer{[&] {
        for (std::uint64_t i = 1; i <= k_publishes; ++i) {
            ring->publish({i, i * 3});
        }
    }};

    constexpr int k_readers = 2;
    std::vector<std::thread> readers;
    readers.reserve(k_readers);
    for (int r = 0; r < k_readers; ++r) {
        readers.emplace_back([&] {
            BroadcastReader reader{ring.get()};
            Event out{};
            std::uint64_t last_seq = 0;
            std::uint64_t items = 0;
            while (last_seq < k_publishes) {
                switch (reader.poll(out)) {
                case PollResult::Item:
                    ASSERT_GT(out.seq, last_seq);        // strictly forward
                    ASSERT_EQ(out.payload, out.seq * 3); // never torn
                    last_seq = out.seq;
                    ++items;
                    break;
                case PollResult::Overrun:
                    last_seq = reader.next_seq() - 1;
                    break;
                case PollResult::Empty:
                    if (last_seq == 0 && reader.next_seq() > k_publishes) {
                        // attached after the end; nothing left to verify
                        last_seq = k_publishes;
                    }
                    break;
                }
            }
            EXPECT_GT(items + 1, 0U); // silence unused warnings; liveness is the assert
        });
    }
    producer.join();
    for (auto& t : readers) {
        t.join();
    }
}

} // namespace
} // namespace raikiri::shm
