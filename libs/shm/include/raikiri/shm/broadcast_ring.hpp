#pragma once

// Broadcast ring: single producer, many independent readers, each with a
// process-private cursor.
//
//   md_events   -> count the gap, jump to head, request a book resync (lossy
//                  by declared policy, but never silently)
//   exec_events -> trading halt for the venue + REST reconciliation (sized so
//                  this never fires; the policy exists so overflow is loud)
//
// A seqlock per slot: item with 1-based sequence s lives
// in slot (s-1) & mask with version 2s when complete, 2s-1 while being
// written. A reader validates the version, word-copies the payload with
// relaxed atomics (see seqlock_table.hpp for why), and re-validates. Any
// mismatch means the producer lapped mid-copy -> overrun, never torn data.

#include <raikiri/common/cacheline.hpp>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace raikiri::shm {

enum class PollResult : std::uint8_t { Empty, Item, Overrun };

template <typename T, std::size_t N>
    requires(std::has_single_bit(N) && std::is_trivially_copyable_v<T> && sizeof(T) % 8 == 0)
struct BroadcastRing {
    static constexpr std::size_t k_capacity = N;
    static constexpr std::uint64_t k_mask = N - 1;
    static constexpr std::size_t k_words = sizeof(T) / 8;

    struct alignas(k_cache_line) Slot {
        std::atomic<std::uint64_t> version{0};
        std::atomic<std::uint64_t> words[k_words];
    };

    alignas(k_pad_align) std::atomic<std::uint64_t> published{0};
    alignas(k_pad_align) Slot slots[N];

    void publish(const T& item) noexcept {
        const auto seq = published.load(std::memory_order_relaxed) + 1;
        Slot& slot = slots[(seq - 1) & k_mask];

        slot.version.store(2 * seq - 1, std::memory_order_relaxed); // writing
        std::atomic_thread_fence(std::memory_order_release);

        std::uint64_t src[k_words];
        std::memcpy(src, &item, sizeof(T));
        for (std::size_t i = 0; i < k_words; ++i) {
            slot.words[i].store(src[i], std::memory_order_relaxed);
        }
        slot.version.store(2 * seq, std::memory_order_release); // complete
        published.store(seq, std::memory_order_release);
    }
};

// Process-private reader cursor. Attach at head; poll() advances one item at
// a time. After Overrun, gap() reports how many items were missed and the
// cursor has already jumped to the current head.
template <typename T, std::size_t N> class BroadcastReader {
public:
    explicit BroadcastReader(const BroadcastRing<T, N>* ring) noexcept
        : ring_{ring}, next_{ring->published.load(std::memory_order_acquire) + 1} {}

    [[nodiscard]] PollResult poll(T& out) noexcept {
        const auto& slot = ring_->slots[(next_ - 1) & BroadcastRing<T, N>::k_mask];
        const auto v1 = slot.version.load(std::memory_order_acquire);
        if (v1 < 2 * next_) {
            return PollResult::Empty; // not yet written (or still writing)
        }
        if (v1 > 2 * next_) {
            return overrun();
        }

        std::uint64_t dst[BroadcastRing<T, N>::k_words];
        for (std::size_t i = 0; i < BroadcastRing<T, N>::k_words; ++i) {
            dst[i] = slot.words[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.version.load(std::memory_order_relaxed) != 2 * next_) {
            return overrun(); // producer lapped mid-copy
        }
        std::memcpy(&out, dst, sizeof(T));
        ++next_;
        return PollResult::Item;
    }

    // Items skipped by the most recent overrun.
    [[nodiscard]] std::uint64_t gap() const noexcept { return gap_; }

    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_; }

private:
    [[nodiscard]] PollResult overrun() noexcept {
        const auto head = ring_->published.load(std::memory_order_acquire);
        gap_ = head >= next_ ? head - next_ + 1 : 0;
        next_ = head + 1;
        return PollResult::Overrun;
    }

    const BroadcastRing<T, N>* ring_;
    std::uint64_t next_; // 1-based sequence this reader expects
    std::uint64_t gap_{0};
};

} // namespace raikiri::shm
