#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/net/ws_limits.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::lighter {

// The subscribes sent and not yet answered, so a 30009 can name the one it refused.
// The pace itself is the IP's message limit in venues.yaml, which the pool spends.
class Outbox {
public:
    explicit Outbox(net::IpMeters& meters) noexcept : meters_{&meters} {}

    [[nodiscard]] bool admit(std::string_view frame, MonoNanos now);
    void answered(std::string_view venue_channel);
    // The channel to ask for again, when the refusal was for pace.
    [[nodiscard]] std::optional<std::string> refused(std::int64_t code, MonoNanos now);

    void reopened() noexcept { unanswered_.clear(); }

    [[nodiscard]] std::size_t unanswered() const noexcept { return unanswered_.size(); }

private:
    net::IpMeters* meters_;
    std::deque<std::string> unanswered_;
};

} // namespace raikiri::venues::lighter
