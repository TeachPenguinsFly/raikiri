#pragma once

// ULID: 26-char Crockford base32, 48-bit millisecond timestamp + 80-bit
// randomness. Used for exchange client order ids: lexically sortable by time,
// unique without coordination, accepted by every venue's client-order-id
// field. Monotonic within one generator: same-millisecond calls increment the
// random component, so ids from one OMS never collide or reorder.
//
// Not cryptographic randomness — these are order identifiers, not secrets.

#include <raikiri/common/fixed_string.hpp>
#include <raikiri/common/time.hpp>

#include <cstdint>

namespace raikiri {

using Ulid = FixedString<26>;

class UlidGen {
public:
    // Seeds from the OS entropy source once at construction (cold path).
    UlidGen();

    // For deterministic tests.
    explicit UlidGen(std::uint64_t seed) noexcept;

    [[nodiscard]] Ulid next(UnixNanos wall) noexcept;

private:
    [[nodiscard]] std::uint64_t next_random() noexcept;

    std::uint64_t state_[4]{};
    std::uint64_t last_ms_{0};
    std::uint64_t rand_hi_{0}; // 16 bits used
    std::uint64_t rand_lo_{0};
};

} // namespace raikiri
