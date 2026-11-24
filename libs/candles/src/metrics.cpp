#include <raikiri/candles/metrics.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace raikiri::candles {

std::optional<std::int32_t> metric_interval(std::string_view text) noexcept {
    static constexpr std::array<std::pair<std::string_view, std::int32_t>, 5> k_intervals{
        {{"5m", 5}, {"15m", 15}, {"1h", 60}, {"4h", 240}, {"1d", 1440}}};
    const auto found = std::ranges::find(k_intervals, text, &std::pair<std::string_view, std::int32_t>::first);
    if (found == k_intervals.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<MetricSample> bucket_metrics(std::span<const MetricSample> samples, std::int32_t minutes) {
    std::vector<MetricSample> out;
    for (const auto& s : samples) {
        const auto open = s.minute / minutes * minutes;
        if (out.empty() || out.back().minute != open) {
            out.push_back({.instrument = s.instrument, .minute = open});
        }
        auto& bucket = out.back();
        const auto keep = [](std::optional<std::int64_t>& held, const std::optional<std::int64_t>& next) {
            if (next.has_value()) {
                held = next;
            }
        };
        keep(bucket.funding_rate, s.funding_rate);
        keep(bucket.mark, s.mark);
        keep(bucket.index, s.index);
        keep(bucket.open_interest, s.open_interest);
    }
    return out;
}

} // namespace raikiri::candles
