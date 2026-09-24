#pragma once

// RateLimiter refuses rather than sleeps; polls may not draw below `reserve`
// InFlightTable ages out unanswered orders so the adapter can synthesize a terminal event

#include <raikiri/common/time.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::exec {

// a refusal; `retry_after_ns` is advice for the caller's backoff
struct RateDenied {
    std::uint64_t retry_after_ns{0};
};

// what a request spends on; only orders may spend the reserve
enum class RateClass : std::uint8_t {
    Order, // create, cancel, amend — may spend the reserve
    Poll,  // catalogue, positions, balances, ledgers — may not
};

// token bucket smoothing within a window, plus a per-window ceiling matching the venue's rule
class RateLimiter {
public:
    // `reserve` is the floor a Poll may not spend below
    RateLimiter(std::uint32_t capacity,
                std::uint64_t refill_window_ns,
                std::uint32_t reserve = 0) noexcept
        : capacity_{capacity}, window_ns_{refill_window_ns}, reserve_{reserve}, tokens_{capacity} {}

    [[nodiscard]] std::expected<void, RateDenied>
    take(std::uint32_t cost, MonoNanos now, RateClass cls = RateClass::Order) noexcept;

    [[nodiscard]] std::uint32_t available(MonoNanos now, RateClass cls = RateClass::Order) noexcept;

    void penalize(MonoNanos now, std::uint64_t backoff_ns) noexcept;

    [[nodiscard]] std::uint64_t denials() const noexcept { return denials_; }

private:
    void refill(MonoNanos now) noexcept;

    std::uint32_t capacity_;
    std::uint64_t window_ns_;
    std::uint32_t reserve_{0};
    std::uint32_t tokens_;
    std::uint64_t last_refill_ns_{0};
    bool started_{false};
    std::uint64_t blocked_until_ns_{0};
    std::uint64_t denials_{0};

    // spent since the current window opened, and when it opened
    std::uint32_t window_spent_{0};
    std::uint64_t window_start_ns_{0};
};

// one request awaiting a venue verdict
struct InFlight {
    ClientOrderId client_order_id{};
    // the id the venue knows it by
    ExchangeClientOrderId exchange_client_order_id{};
    InstrumentId instrument{};
    // enough to describe the order without the store
    Side side{Side::Buy};
    QtyLots qty{};
    CmdKind kind{CmdKind::Create};
    MonoNanos sent{};
};

// requests between the transport and the venue's answer; small, so a linear scan
class InFlightTable {
public:
    explicit InFlightTable(std::size_t capacity) { entries_.reserve(capacity); }

    [[nodiscard]] bool add(const InFlight& request) noexcept;

    bool resolve(ClientOrderId id) noexcept;

    [[nodiscard]] std::vector<InFlight> expire(MonoNanos now, std::uint64_t timeout_ns);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] std::uint64_t expired_total() const noexcept { return expired_total_; }

private:
    std::vector<InFlight> entries_;
    std::uint64_t expired_total_{0};
};

} // namespace raikiri::exec
