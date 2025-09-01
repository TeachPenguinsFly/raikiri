#pragma once

// Venue-agnostic order plumbing: what every ExecutionClient needs before it
// may send a live order, and what none of them should write twice.
//
// RateLimiter. Every venue meters differently — binance charges WEIGHT per
// endpoint against a per-minute budget and counts orders separately, bybit
// meters per endpoint, bitmex allows a flat 180/min — so the limiter takes a
// cost per request instead of assuming one. It refuses rather than sleeps: a
// gateway that blocks in submit() stalls the loop that also drains cancels,
// and an order delayed behind a rate limit is worse than an order refused
// while the escape hatch still works.
//
// THE RESERVE IS WHY THE CLASS OF A REQUEST MATTERS. Reconciliation pulls are
// numerous, individually cheap to delay and expensive to skip; order traffic is
// rare and cannot wait. Sharing one flat bucket lets a poll cycle spend the
// budget a cancel is about to need — and a cancel that cannot be sent is an
// open position the strategy believes is closed. Polls therefore may not draw
// the bucket below `reserve`, which only order traffic may spend.
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

// What a request is spending on. The distinction exists for the reserve.
enum class RateClass : std::uint8_t {
    Order, // create, cancel, amend — may spend the reserve
    Poll,  // catalogue, positions, balances, ledgers — may not
};

// Token bucket over a fixed window. Costs are venue units: 1 request, or
// binance's endpoint weight, or an order count against a separate budget.
class RateLimiter {
public:
    // `reserve` is the floor a Poll may not spend below. Zero is right only
    // where nothing but order traffic shares the budget.
    RateLimiter(std::uint32_t capacity,
                std::uint64_t refill_window_ns,
                std::uint32_t reserve = 0) noexcept
        : capacity_{capacity}, window_ns_{refill_window_ns}, reserve_{reserve}, tokens_{capacity} {}

    [[nodiscard]] std::expected<void, RateDenied>
    take(std::uint32_t cost, MonoNanos now, RateClass cls = RateClass::Order) noexcept;

    [[nodiscard]] std::uint32_t available(MonoNanos now,
                                          RateClass cls = RateClass::Order) noexcept;

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
