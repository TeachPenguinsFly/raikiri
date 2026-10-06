#pragma once

// one request answers every market; only the instruments asked for reach the sink

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <unordered_set>

namespace raikiri::exec {

inline constexpr std::uint64_t k_ticker_poll_interval_ns = 60'000'000'000ULL;

class TickerPoll {
public:
    explicit TickerPoll(std::uint64_t interval_ns = k_ticker_poll_interval_ns) noexcept : interval_ns_{interval_ns} {}

    void want(InstrumentId id) { wanted_.insert(id); }

    [[nodiscard]] bool wanted(InstrumentId id) const noexcept { return wanted_.contains(id); }

    [[nodiscard]] bool empty() const noexcept { return wanted_.empty(); }

    // `send` returns false when refused, which leaves the poll due for the next tick
    template <typename Send> void poll(UnixNanos now, Send&& send) noexcept {
        if (wanted_.empty() || in_flight_ || now.ns < next_ns_) {
            return;
        }
        in_flight_ = true;
        bool sent = false;
        try {
            sent = send();
        } catch (...) {
            sent = false;
        }
        if (!sent) {
            in_flight_ = false;
            return;
        }
        next_ns_ = now.ns + interval_ns_;
    }

    // call first in the reply handler, failed replies included
    void answered() noexcept { in_flight_ = false; }

private:
    std::uint64_t interval_ns_;
    std::uint64_t next_ns_{0};
    bool in_flight_{false};
    std::unordered_set<InstrumentId> wanted_;
};

} // namespace raikiri::exec
