#pragma once

#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::candles {

// what one instrument's funding and ticker cells held at the start of a minute; decimal e9, the open interest in the
// base asset. A field is empty where the venue publishes none
struct MetricSample {
    InstrumentId instrument{};
    std::int32_t minute{0}; // unix minutes
    std::optional<std::int64_t> funding_rate{};
    std::optional<std::int64_t> mark{};
    std::optional<std::int64_t> index{};
    std::optional<std::int64_t> open_interest{};

    [[nodiscard]] constexpr bool empty() const noexcept {
        return !funding_rate.has_value() && !mark.has_value() && !index.has_value() && !open_interest.has_value();
    }
};

inline constexpr std::size_t k_metric_default_limit = 1000;
inline constexpr std::size_t k_metric_max_limit = 1000;

// minutes in a chart interval: 5m, 15m, 1h, 4h, 1d; nothing for any other text
[[nodiscard]] std::optional<std::int32_t> metric_interval(std::string_view text) noexcept;

[[nodiscard]] constexpr std::size_t metric_limit(std::optional<std::uint64_t> asked) noexcept {
    const auto limit = asked.value_or(k_metric_default_limit);
    return limit < k_metric_max_limit ? static_cast<std::size_t>(limit) : k_metric_max_limit;
}

// the minutes [from, to) that hold the newest `limit` buckets of `minutes` each, the one `now_minute` falls in included
struct MetricWindow {
    std::int32_t from{0};
    std::int32_t to{0};
};

[[nodiscard]] constexpr MetricWindow
metric_window(std::int32_t now_minute, std::int32_t minutes, std::size_t limit) noexcept {
    const auto to = now_minute / minutes * minutes + minutes;
    return {.from = to - static_cast<std::int32_t>(limit) * minutes, .to = to};
}

// one instrument's samples, oldest first, folded into buckets of `minutes`: `minute` becomes the bucket's open and each
// field its last sample that had one. A bucket without samples yields nothing
[[nodiscard]] std::vector<MetricSample> bucket_metrics(std::span<const MetricSample> samples, std::int32_t minutes);

} // namespace raikiri::candles
