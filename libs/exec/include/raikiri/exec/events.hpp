#pragma once

// The single event boundary between a venue adapter and the gateway core.
//
// Hot-path contract: implementations are wait-free (they write seqlock
// tables / broadcast rings / journal appends) and never throw; adapters may
// call from their I/O thread at full tick rate.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/error.hpp>

#include <cstdint>

namespace raikiri::exec {

enum class ConnState : std::uint8_t {
    Connected = 0,
    Disconnected,
    AuthFailed,
    Resubscribed, // reconnect completed and subscriptions restored
};

struct ConnEvent {
    ConnState state{ConnState::Disconnected};
    std::uint32_t stream_id{0}; // adapter-local: which socket within the venue
    UnixNanos ts{};
};

class GatewaySink {
public:
    GatewaySink() = default;
    GatewaySink(const GatewaySink&) = delete;
    GatewaySink& operator=(const GatewaySink&) = delete;

    virtual void on_bbo(const Bbo& bbo) noexcept = 0;
    virtual void on_trade(const TradeTick& trade) noexcept = 0;

    // Declining by default: most venues publish depth, but an adapter that
    // does not maintain a book must not be forced to invent one.
    virtual void on_depth(const BookSnapshot& book) noexcept { (void)book; }

    virtual void on_funding(const FundingUpdate& funding) noexcept = 0;

    // Declining by default: one venue publishes greeks today, and an adapter
    // for a venue that lists no options has nothing to say here.
    virtual void on_option_greeks(const OptionGreeks& greeks) noexcept { (void)greeks; }
    virtual void on_order_event(const OrderEvent& event) noexcept = 0;

    // A REFUSAL WITHOUT ITS REASON IS UNDIAGNOSABLE. `OrderEvent` carries no
    // text — it is the 192-byte shm payload — so the venue's own verdict comes
    // through here, paired to the terminal event by client order id. Declining
    // by default, because a sink that only books state has nowhere to put it;
    // an adapter that was handed a reason must never drop it.
    virtual void on_order_error(ClientOrderId client_order_id,
                                const VenueError& error) noexcept {
        (void)client_order_id;
        (void)error;
    }
    virtual void on_connectivity(const ConnEvent& event) noexcept = 0;

protected:
    ~GatewaySink() = default; // interface: never owned through the base
};

} // namespace raikiri::exec
