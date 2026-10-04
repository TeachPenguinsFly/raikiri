#pragma once

#include <raikiri/introspect/latency.hpp>
#include <raikiri/shm/records.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

#include "latency_meter.hpp"

namespace raikiri::gatewayd {

struct ClockState {
    introspect::ClockEstimate estimate{};
    std::uint64_t failures{0};
};

struct FeedTotals {
    std::uint64_t catalogue{0};
    std::uint64_t priced{0};
    std::uint64_t reopens{0};
};

[[nodiscard]] inline shm::VenueMetrics venue_metrics(const ClockState& clock,
                                                     std::string_view endpoint,
                                                     const LatencyMeter& meter,
                                                     const FeedTotals& totals,
                                                     std::uint64_t mono_ns,
                                                     std::uint64_t wall_ns) noexcept {
    const auto& estimate = clock.estimate;
    shm::VenueMetrics out{};
    out.published_ns = wall_ns;
    out.clock_source = estimate.method == introspect::ClockMethod::RestProbe ? shm::ClockSource::RestProbe
                                                                             : shm::ClockSource::None;
    out.probe_failures = static_cast<std::uint32_t>(clock.failures);
    out.offset_ns = estimate.offset_ns;
    out.rtt_ns = estimate.rtt_ns;
    out.uncertainty_ns = estimate.uncertainty_ns;
    out.measured_ns = estimate.sampled_ns;
    out.drift_ns = estimate.drift_ns;
    out.drift_over_ns = estimate.drift_over_ns;
    out.catalogue = totals.catalogue;
    out.priced = totals.priced;
    out.reopens = totals.reopens;
    const auto copied = std::min(endpoint.size(), sizeof out.endpoint - 1);
    std::copy_n(endpoint.data(), copied, out.endpoint);
    for (std::size_t i = 0; i < shm::k_metric_channels; ++i) {
        const auto feed = meter.feed(i, mono_ns);
        out.channels[i] = shm::ChannelMetrics{.messages = feed.messages,
                                              .window_ns = feed.window_ns,
                                              .lag_samples = feed.lag_samples,
                                              .lag_p50_ns = feed.lag_p50_ns,
                                              .lag_p99_ns = feed.lag_p99_ns,
                                              .last_recv_ns = feed.last_recv_ns};
    }
    return out;
}

} // namespace raikiri::gatewayd
