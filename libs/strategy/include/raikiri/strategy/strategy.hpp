#pragma once

// The public extension point: everything a plugged-in strategy can see and do.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/portfolio/position.hpp>

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

    [[nodiscard]] virtual std::expected<ClientOrderId, RejectReason>
    submit(const NewOrder& order) noexcept = 0;

    virtual bool cancel(ClientOrderId id) noexcept = 0;

    virtual void cancel_all(InstrumentId instrument) noexcept = 0;

    // Latest known state; nullopt before the first tick.
    [[nodiscard]] virtual std::optional<Bbo> bbo(InstrumentId instrument) const noexcept = 0;

    [[nodiscard]] virtual const Instrument* instrument(InstrumentId id) const noexcept = 0;

    // The caller's own position, null before its first fill. The kernel
    // supplies the strategy_id, so a strategy cannot read another's book.
    [[nodiscard]] virtual const portfolio::Position*
    position(InstrumentId instrument) const noexcept = 0;

    // Deliberately takes an explicit mark rather than defaulting to the mid:
    // which price marks a position is a strategy decision, and a stored
    // unrealized number can only answer "as of when?" wrongly.
    [[nodiscard]] virtual FixedE9 unrealized(InstrumentId instrument,
                                             PriceTicks mark) const noexcept = 0;

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
