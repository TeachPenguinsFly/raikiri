#pragma once

// bounded, lock-free, preallocated: many producers, one consumer; a full queue refuses and never waits

#include <raikiri/audit/log.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace raikiri::audit {

class EntryQueue {
public:
    // a label longer than this is cut, and a detail with no room left becomes {"truncated":true,"bytes":N}
    static constexpr std::size_t k_max_label_bytes = 256;
    static constexpr std::size_t k_min_slot_bytes = (k_max_label_bytes * 3) + 64;

    // `slots` rounds up to a power of two
    EntryQueue(std::size_t slots, std::size_t slot_bytes);

    // false when full; allocates nothing, and on success sets `truncated` if the entry had to be cut
    [[nodiscard]] bool try_push(const Entry& entry, bool& truncated) noexcept;

    // the oldest entry, valid until consume; only the consumer thread may call these
    [[nodiscard]] bool peek(Entry& entry) const noexcept;
    void consume() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Header {
        std::atomic<std::uint64_t> seq{0};
        std::uint64_t ts{0};
        std::uint16_t actor{0};
        std::uint16_t action{0};
        std::uint16_t subject{0};
        std::uint32_t detail{0};
    };

    std::size_t mask_;
    std::size_t slot_bytes_;
    std::unique_ptr<Header[]> headers_;
    std::unique_ptr<char[]> bytes_;
    alignas(64) std::atomic<std::uint64_t> enqueue_{0};
    alignas(64) std::uint64_t dequeue_{0};
};

} // namespace raikiri::audit
