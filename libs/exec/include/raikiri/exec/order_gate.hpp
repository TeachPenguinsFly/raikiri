#pragma once

// Venue-agnostic order plumbing: what every ExecutionClient needs before it
// may send a live order, and what none of them should write twice.
//
// Two pieces, both extracted from working adapters rather than imagined.
//
// RateLimiter. Every venue meters differently — binance charges WEIGHT per
// endpoint against a per-minute budget and counts orders separately, bybit
// meters per endpoint, bitmex allows a flat 180/min — so the limiter takes a
// cost per request instead of assuming one. It refuses rather than sleeps: a
// gateway that blocks in submit() stalls the loop that also drains cancels,
// and an order delayed behind a rate limit is worse than an order refused
// while the escape hatch still works.
//
// InFlightTable. An order handed to a venue and never answered is the failure
// this exists for. REST can time out after the exchange accepted the order,
// so the verdict is unknown rather than negative — but leaving it unknown
// forever means the OMS holds an order that no event will ever close. The
// table ages requests out and hands them back so the adapter can synthesize a
// terminal event and let reconciliation establish the truth.

#include <raikiri/common/time.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::exec {

// A refusal, not a delay. `retry_after_ns` is what the caller should wait
// before asking again; it is advice for the backoff, not a sleep taken here.
struct RateDenied {
    std::uint64_t retry_after_ns{0};
};

// Token bucket over a fixed window. Costs are venue units: 1 request, or
// binance's endpoint weight, or an order count against a separate budget.
class RateLimiter {
public:
    RateLimiter(std::uint32_t capacity, std::uint64_t refill_window_ns) noexcept
        : capacity_{capacity}, window_ns_{refill_window_ns}, tokens_{capacity} {}

    // Continuous refill rather than a window that resets on the minute: a
    // venue's clock and ours never agree on where the boundary is, and a
    // burst spanning a reset is how an account discovers a ban.
    [[nodiscard]] std::expected<void, RateDenied> take(std::uint32_t cost, MonoNanos now) noexcept;

    [[nodiscard]] std::uint32_t available(MonoNanos now) noexcept;

    // A venue that answered 429 knows better than our accounting. Draining
    // makes the next take() refuse until the bucket genuinely refills.
    void penalize(MonoNanos now, std::uint64_t backoff_ns) noexcept;

    [[nodiscard]] std::uint64_t denials() const noexcept { return denials_; }

private:
    void refill(MonoNanos now) noexcept;

    std::uint32_t capacity_;
    std::uint64_t window_ns_;
    std::uint32_t tokens_;
    std::uint64_t last_refill_ns_{0};
    // Separate flag rather than treating 0 as "unset": zero is a legitimate
    // MonoNanos, and conflating the two made the first window refill nothing.
    bool started_{false};
    std::uint64_t blocked_until_ns_{0};
    std::uint64_t denials_{0};
};

// One request awaiting a venue verdict.
struct InFlight {
    ClientOrderId client_order_id{};
    InstrumentId instrument{};
    CmdKind kind{CmdKind::Create};
    MonoNanos sent{};
};

// Tracks requests between "handed to the transport" and "the venue answered".
// Deliberately small and flat: a live gateway holds tens of these, and a hash
// map would cost more than the linear scan it replaces.
class InFlightTable {
public:
    explicit InFlightTable(std::size_t capacity) { entries_.reserve(capacity); }

    // False when the table is full, which the caller must treat as venue-down
    // rather than sending anyway — an untracked order is one that can never
    // be aged out.
    [[nodiscard]] bool add(const InFlight& request) noexcept;

    // Called when a verdict arrives. Returns false for an id we are not
    // tracking, which is normal: the venue also reports fills for orders
    // placed before this process started.
    bool resolve(ClientOrderId id) noexcept;

    // Requests older than `timeout_ns`, removed from the table. The caller
    // synthesizes a terminal event for each — the order's real fate is then
    // reconciliation's problem, which is the only component that can ask the
    // venue what actually happened.
    [[nodiscard]] std::vector<InFlight> expire(MonoNanos now, std::uint64_t timeout_ns);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] std::uint64_t expired_total() const noexcept { return expired_total_; }

private:
    std::vector<InFlight> entries_;
    std::uint64_t expired_total_{0};
};

} // namespace raikiri::exec
