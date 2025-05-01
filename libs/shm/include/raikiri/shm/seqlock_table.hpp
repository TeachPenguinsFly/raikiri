#pragma once

// Seqlock table: single-writer, many-reader, overwrite-newest state cells.
//
// Memory-model correctness: the payload is stored as an array of
// std::atomic<uint64_t> words written/read with relaxed ordering between the
// version bumps. The classic "memcpy the payload" seqlock is a data race in
// the C++ abstract machine even though it happens to work; word-wise relaxed
// atomics compile to the same plain loads/stores on x86-64 and arm64, are
// defined behavior, and let TSan verify these tests instead of being
// suppressed. Odd version = write in progress.

#include <raikiri/common/cacheline.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace raikiri::shm {

template <typename T>
    requires(std::is_trivially_copyable_v<T> && sizeof(T) % 8 == 0)
struct alignas(k_cache_line) SeqlockCell {
    static constexpr std::size_t k_words = sizeof(T) / 8;

    std::atomic<std::uint32_t> version{0};
    std::uint32_t pad_{0};
    std::atomic<std::uint64_t> words[k_words];

    void write(const T& value) noexcept {
        const auto v = version.load(std::memory_order_relaxed);
        version.store(v + 1, std::memory_order_relaxed); // odd: in progress
        std::atomic_thread_fence(std::memory_order_release);

        std::uint64_t src[k_words];
        std::memcpy(src, &value, sizeof(T));
        for (std::size_t i = 0; i < k_words; ++i) {
            words[i].store(src[i], std::memory_order_relaxed);
        }
        version.store(v + 2, std::memory_order_release);
    }

    [[nodiscard]] std::uint32_t read(T& out) const noexcept {
        std::uint64_t dst[k_words];
        for (;;) {
            const auto v1 = version.load(std::memory_order_acquire);
            if (v1 == 0) {
                return 0;
            }
            if ((v1 & 1U) != 0) {
                continue; // writer mid-flight; the window is a few stores
            }
            for (std::size_t i = 0; i < k_words; ++i) {
                dst[i] = words[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version.load(std::memory_order_relaxed) == v1) {
                std::memcpy(&out, dst, sizeof(T));
                return v1;
            }
        }
    }

    // Change-gate probe without copying the payload.
    [[nodiscard]] std::uint32_t version_now() const noexcept {
        return version.load(std::memory_order_acquire);
    }
};

template <typename T, std::size_t N> struct SeqlockTable {
    static constexpr std::size_t k_capacity = N;

    SeqlockCell<T> cells[N];

    void write(std::size_t index, const T& value) noexcept { cells[index].write(value); }

    [[nodiscard]] std::uint32_t read(std::size_t index, T& out) const noexcept {
        return cells[index].read(out);
    }
};

} // namespace raikiri::shm
