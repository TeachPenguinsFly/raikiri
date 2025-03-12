#pragma once

// The public extension point: everything a plugged-in strategy can see and
// do. Private strategies (funding-arb, market-making) link exactly this
// header plus the domain vocabulary — never the kernel, never venues, never
// transports. That boundary is what lets a closed-source strategy ride the
// open framework.
//
// Callbacks receive Context& per call instead of storing a reference:
// lifetimes stay trivial, and the same strategy object is reusable across
// engine restarts (crash-only kernels recreate the context, not the
// strategy).
//
// Everything here is hot-path adjacent: noexcept, fixed-point, no
// allocation implied. Timers and data callbacks arrive on ONE thread — the
// kernel is single-threaded by design, so strategies need no locks, ever.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_events.hpp>

#include <cstdint>
#include <expected>
#include <optional>

namespace raikiri::strategy {

struct NewOrder {
    InstrumentId instrument{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Gtc};
    OrderFlags flags{OrderFlags::None};
    PriceTicks price{}; // ignored for Market
    QtyLots qty{};
};

enum class RejectReason : std::uint8_t {
    Risk,      // risk gate refused (details in the journal/logs)
    Capacity,  // order store full — a strategy leak, treat as a bug
    VenueDown, // command ring closed/full: gateway dead or halted
};

class Context {
public:
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Fire an order. Success returns the ClientOrderId to correlate every
    // later on_order_event; the Submitted/Accepted/... lifecycle arrives
    // asynchronously. Denials also arrive as Denied events — the expected
    // here is the synchronous fast-fail only.
    [[nodiscard]] virtual std::expected<ClientOrderId, RejectReason>
    submit(const NewOrder& order) noexcept = 0;

    // True if the cancel was dispatched (not that it succeeded — watch for
    // Canceled/CancelRejected events).
    virtual bool cancel(ClientOrderId id) noexcept = 0;

    virtual void cancel_all(InstrumentId instrument) noexcept = 0;

    // Latest known state; nullopt before the first tick.
    [[nodiscard]] virtual std::optional<Bbo> bbo(InstrumentId instrument) const noexcept = 0;

    // Borrowed from the engine's immutable instrument table; nullptr for
    // unknown ids. Valid for the engine's lifetime.
    [[nodiscard]] virtual const Instrument* instrument(InstrumentId id) const noexcept = 0;

    // One-shot timer; fires on_timer(timer_id) on the kernel thread.
    // Re-arming an already-pending id replaces it.
    virtual void set_timer(std::uint32_t timer_id, std::uint64_t delay_ns) noexcept = 0;

    [[nodiscard]] virtual const Clock& clock() const noexcept = 0;

protected:
    ~Context() = default; // interface: never owned through the base
};

class Strategy {
public:
    Strategy() = default;
    Strategy(const Strategy&) = delete;
    Strategy& operator=(const Strategy&) = delete;
    virtual ~Strategy() = default;

    // Defaults are all no-ops: a strategy overrides exactly the events it
    // trades on (same philosophy as DataClient's declining defaults).
    virtual void on_start(Context& ctx) { (void)ctx; }

    virtual void on_stop(Context& ctx) { (void)ctx; }

    virtual void on_bbo(Context& ctx, const Bbo& bbo) {
        (void)ctx;
        (void)bbo;
    }

    virtual void on_trade(Context& ctx, const TradeTick& trade) {
        (void)ctx;
        (void)trade;
    }

    virtual void on_funding(Context& ctx, const FundingUpdate& funding) {
        (void)ctx;
        (void)funding;
    }

    virtual void on_order_event(Context& ctx, const OrderEvent& event) {
        (void)ctx;
        (void)event;
    }

    virtual void on_timer(Context& ctx, std::uint32_t timer_id) {
        (void)ctx;
        (void)timer_id;
    }
};

} // namespace raikiri::strategy
