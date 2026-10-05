#pragma once

// where the venue clock can be, as hard bounds on venue minus local; integer nanoseconds

#include <raikiri/introspect/latency.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace raikiri::introspect {

inline constexpr std::int64_t k_open_below = std::numeric_limits<std::int64_t>::min();
inline constexpr std::int64_t k_open_above = std::numeric_limits<std::int64_t>::max();

struct OffsetInterval {
    std::int64_t lo{k_open_below};
    std::int64_t hi{k_open_above};

    [[nodiscard]] bool empty() const noexcept { return lo > hi; }

    friend bool operator==(const OffsetInterval&, const OffsetInterval&) = default;
};

// may come back empty; the caller decides what disagreement means
[[nodiscard]] OffsetInterval intersect(OffsetInterval a, OffsetInterval b) noexcept;

// the server stamped between our send and receive, truncated to its resolution
[[nodiscard]] OffsetInterval sample_interval(const OffsetSample& sample) noexcept;

[[nodiscard]] OffsetInterval rest_interval(const ClockEstimate& estimate) noexcept;

// the point nearest zero: our clock is the better one, so move it only as far as the bounds force
[[nodiscard]] std::int64_t least_correction(OffsetInterval interval) noexcept;

enum class OffsetSource : std::uint8_t { None, Rest, Feed, RestAndFeed, Conflict };

[[nodiscard]] std::string_view to_string(OffsetSource source) noexcept;

struct CombinedOffset {
    OffsetSource source{OffsetSource::None};
    // lo > hi on a conflict: the feed bound above the probe's upper edge
    OffsetInterval interval{};
    // added to ts_recv - ts_event; zero without evidence or on a conflict
    std::int64_t correction_ns{0};
};

// feed_lo is the largest ts_event - ts_recv seen, a lower bound while one-way delay is not negative
[[nodiscard]] CombinedOffset combine(const ClockEstimate& rest, std::optional<std::int64_t> feed_lo) noexcept;

} // namespace raikiri::introspect
