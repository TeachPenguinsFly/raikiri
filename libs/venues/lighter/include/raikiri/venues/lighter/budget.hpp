#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/exec/order_gate.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::lighter {

// Every socket message this process sends to lighter, at the IP's rate, in bursts
// small enough that no sliding minute holds more than per_minute plus one burst.
class MessageBudget {
public:
    explicit MessageBudget(std::uint32_t per_minute) noexcept;

    [[nodiscard]] bool take(MonoNanos now) noexcept;
    void hold(MonoNanos now, std::uint64_t hold_ns) noexcept;

    [[nodiscard]] std::uint32_t per_minute() const noexcept { return per_minute_; }

    [[nodiscard]] std::uint32_t burst() const noexcept { return burst_; }

private:
    std::mutex mu_;
    std::uint32_t per_minute_;
    std::uint32_t burst_;
    exec::RateLimiter limiter_;
};

// ep::k_ws_messages_per_minute unless the environment names another.
[[nodiscard]] std::uint32_t ws_messages_per_minute() noexcept;

// One per process, so a reconnect spends what the dropped socket left.
[[nodiscard]] std::shared_ptr<MessageBudget> process_budget();

// The venue answers one socket's messages in order and a refusal names no channel,
// so a refusal belongs to the oldest subscribe not yet answered.
class Outbox {
public:
    explicit Outbox(std::shared_ptr<MessageBudget> budget) noexcept : budget_{std::move(budget)} {}

    [[nodiscard]] bool admit(std::string_view frame, MonoNanos now);
    void answered(std::string_view venue_channel);
    // The channel to ask for again, for a rate refusal only.
    [[nodiscard]] std::optional<std::string> refused(std::int64_t code, MonoNanos now);

    void reopened() noexcept { unanswered_.clear(); }

    [[nodiscard]] std::size_t unanswered() const noexcept { return unanswered_.size(); }

private:
    std::shared_ptr<MessageBudget> budget_;
    std::deque<std::string> unanswered_;
};

} // namespace raikiri::venues::lighter
