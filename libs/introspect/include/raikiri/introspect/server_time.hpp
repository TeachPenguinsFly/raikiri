#pragma once

// each venue's public rest time endpoint; venues without one are bounded from the feed

#include <raikiri/introspect/clock_probe.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri {
class Clock;
}

namespace raikiri::introspect {

struct TimeEndpoint {
    std::string_view venue;
    std::string_view market; // empty serves every market
    std::string_view host;
    std::string_view path;
    std::string_view key; // empty: the body is the number
    std::uint64_t unit_ns{1'000'000};
};

[[nodiscard]] std::optional<TimeEndpoint> time_endpoint(std::string_view venue,
                                                        std::string_view market) noexcept;

// the first number after `"key":`, quoted or not, in `unit_ns`
[[nodiscard]] std::optional<std::uint64_t>
parse_server_time(std::string_view body, std::string_view key, std::uint64_t unit_ns) noexcept;

class RestServerTime final : public ServerTimeSource {
public:
    RestServerTime(TimeEndpoint endpoint, const Clock& clock);
    ~RestServerTime() override;

    [[nodiscard]] std::string endpoint() const override;
    [[nodiscard]] std::optional<OffsetSample> read() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::introspect
