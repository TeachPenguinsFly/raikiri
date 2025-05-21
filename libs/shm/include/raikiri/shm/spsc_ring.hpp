#pragma once

// Bounded lossless SPSC ring for shared memory.
//
// Two standard defences against cache-line traffic, and one consequence of
// living in shared memory:
//
// - head and tail sit on separate padded lines, so the producer's store never
//   invalidates the line the consumer is reading;
// - each side keeps a cached copy of the other's index and only re-reads the
//   real one when the cache says the ring is full or empty, which takes the
//   other side's line out of the common path entirely;
// - those caches are process-private, so they live in the Producer/Consumer
//   handles and NOT in the shared struct. A "private" field inside a segment
//   two processes map is not private, and one that both sides write is a
//   correctness bug, not a slow path.
//
// Counters are monotonic and never wrap; the slot is (counter & mask), which
// needs N to be a power of two and makes "full" a subtraction rather than a
// comparison against a wrapped index.

#include <raikiri/common/cacheline.hpp>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace raikiri::shm {

enum class RingError : std::uint8_t { Full };

template <typename T, std::size_t N>
    requires(std::has_single_bit(N) && std::is_trivially_copyable_v<T>)
struct SpscRing {
    static constexpr std::size_t k_capacity = N;
    static constexpr std::uint64_t k_mask = N - 1;

    alignas(k_pad_align) std::atomic<std::uint64_t> tail{0}; // next write index
    alignas(k_pad_align) std::atomic<std::uint64_t> head{0}; // next read index
    alignas(k_pad_align) T slots[N];
};

template <typename T, std::size_t N> class SpscProducer {
public:
    explicit SpscProducer(SpscRing<T, N>* ring) noexcept
        : ring_{ring}, cached_head_{ring->head.load(std::memory_order_acquire)} {}

    [[nodiscard]] std::expected<void, RingError> try_push(const T& item) noexcept {
        const auto tail = ring_->tail.load(std::memory_order_relaxed);
        if (tail - cached_head_ == N) {
            cached_head_ = ring_->head.load(std::memory_order_acquire);
            if (tail - cached_head_ == N) {
                return std::unexpected{RingError::Full};
            }
        }
        ring_->slots[tail & SpscRing<T, N>::k_mask] = item;
        ring_->tail.store(tail + 1, std::memory_order_release);
        return {};
    }

private:
    SpscRing<T, N>* ring_;
    std::uint64_t cached_head_; // process-private; never in shm
};

template <typename T, std::size_t N> class SpscConsumer {
public:
    explicit SpscConsumer(SpscRing<T, N>* ring) noexcept
        : ring_{ring}, cached_tail_{ring->tail.load(std::memory_order_acquire)} {}

    // Copies the next item into `out`; false when empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const auto head = ring_->head.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            cached_tail_ = ring_->tail.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return false;
            }
        }
        out = ring_->slots[head & SpscRing<T, N>::k_mask];
        ring_->head.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t approx_size() const noexcept {
        return static_cast<std::size_t>(ring_->tail.load(std::memory_order_acquire) -
                                        ring_->head.load(std::memory_order_acquire));
    }

private:
    SpscRing<T, N>* ring_;
    std::uint64_t cached_tail_; // process-private; never in shm
};

} // namespace raikiri::shm
