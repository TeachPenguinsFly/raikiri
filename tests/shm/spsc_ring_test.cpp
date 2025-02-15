#include <raikiri/shm/spsc_ring.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

namespace raikiri::shm {
namespace {

using Ring = SpscRing<std::uint64_t, 1024>;

TEST(SpscRing, PushPopSingleThread) {
    auto ring = std::make_unique<Ring>();
    SpscProducer producer{ring.get()};
    SpscConsumer consumer{ring.get()};

    ASSERT_TRUE(producer.try_push(41).has_value());
    ASSERT_TRUE(producer.try_push(42).has_value());

    std::uint64_t out{};
    ASSERT_TRUE(consumer.try_pop(out));
    EXPECT_EQ(out, 41U);
    ASSERT_TRUE(consumer.try_pop(out));
    EXPECT_EQ(out, 42U);
    EXPECT_FALSE(consumer.try_pop(out)); // empty again
}

TEST(SpscRing, FullReportsErrorNeverDrops) {
    auto ring = std::make_unique<Ring>();
    SpscProducer producer{ring.get()};
    SpscConsumer consumer{ring.get()};

    for (std::uint64_t i = 0; i < Ring::k_capacity; ++i) {
        ASSERT_TRUE(producer.try_push(i).has_value()) << i;
    }
    const auto overflow = producer.try_push(9999);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error(), RingError::Full);

    // Drain one, push succeeds again.
    std::uint64_t out{};
    ASSERT_TRUE(consumer.try_pop(out));
    EXPECT_EQ(out, 0U);
    EXPECT_TRUE(producer.try_push(9999).has_value());
}

// TSan target: full-speed producer/consumer across threads. Any ordering bug
// in the ring is a data race TSan can see because slots are plain (non-
// atomic) memory synchronized purely by the head/tail acquire/release pair.
TEST(SpscRingStress, CrossThreadTransferPreservesSequence) {
    auto ring = std::make_unique<SpscRing<std::uint64_t, 256>>();
    constexpr std::uint64_t k_count = 1'000'000;

    std::thread producer_thread{[&ring] {
        SpscProducer producer{ring.get()};
        for (std::uint64_t i = 0; i < k_count;) {
            if (producer.try_push(i).has_value()) {
                ++i;
            }
        }
    }};

    SpscConsumer consumer{ring.get()};
    std::uint64_t expected = 0;
    while (expected < k_count) {
        std::uint64_t out{};
        if (consumer.try_pop(out)) {
            ASSERT_EQ(out, expected); // lossless AND ordered
            ++expected;
        }
    }
    producer_thread.join();
}

} // namespace
} // namespace raikiri::shm
