#pragma once

// monotonic within one generator: same-millisecond calls increment the random part

#include <raikiri/common/fixed_string.hpp>
#include <raikiri/common/time.hpp>

#include <cstdint>

namespace raikiri {

using Ulid = FixedString<26>;

class UlidGen {
public:
    UlidGen();

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
