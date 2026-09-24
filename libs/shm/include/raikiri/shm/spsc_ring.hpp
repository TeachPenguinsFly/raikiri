#pragma once

// head and tail on separate lines; each side's cached copy of the other's index is process-private
// and stays out of shm

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

private:
    SpscRing<T, N>* ring_;
    std::uint64_t cached_tail_; // process-private; never in shm
};

} // namespace raikiri::shm
