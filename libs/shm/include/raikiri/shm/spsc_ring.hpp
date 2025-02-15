#pragma once

// Bounded lossless SPSC ring for shared memory. One producer process, one
// consumer process. Used for order-command channels (strategy -> gateway),
// where "full" is meaningful backpressure (the gateway is dead or stuck) and
// must surface as an error — never a silent drop, never a spin-wait.
//
// Layout derives from rigtorp::SPSCQueue (MIT, credited in NOTICE): head and
// tail on separate padded lines, and each side keeps a *cached* copy of the
// other side's index to avoid touching its cache line on every operation.
// The twist for shared memory: those caches are process-private state, so
// they live in the Producer/Consumer handles, not in the shared struct —
// two processes must never share a "private" cache.
//
// The shared struct is trivially constructible in place over zeroed mapped
// memory; construction is just the mapping itself.

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

template <typename T, std::size_t N>
class SpscProducer {
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

template <typename T, std::size_t N>
class SpscConsumer {
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
