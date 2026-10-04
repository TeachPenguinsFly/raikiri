#pragma once

// a venue's server-time request, as its adapter describes it, read over a connection of its own

#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/introspect/clock_probe.hpp>

#include <memory>
#include <optional>
#include <string>

namespace raikiri {
class Clock;
}

namespace raikiri::introspect {

class RestServerTime final : public ServerTimeSource {
public:
    RestServerTime(exec::ServerTimeQuery query, const Clock& clock);
    ~RestServerTime() override;

    [[nodiscard]] std::string endpoint() const override;
    [[nodiscard]] std::optional<OffsetSample> read() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::introspect
