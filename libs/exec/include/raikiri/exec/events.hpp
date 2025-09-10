#pragma once

// The single event boundary between a venue adapter and the gateway core.
//
// Hot-path contract: implementations are wait-free (they write seqlock
// tables / broadcast rings / journal appends) and never throw; adapters may
// call from their I/O thread at full tick rate.

#include <raikiri/domain/ledger_entry.hpp>
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

    // What the venue CHARGED, as opposed to the rate above, which is what it
    // said it would charge. Declining by default and paired with the
    // `funding_charge` capability bit: an adapter that cannot read its venue's
    // account ledger must stay silent here rather than let a caller mistake
    // silence for nothing having been charged.
    virtual void on_funding_charge(const LedgerEntry& charge) noexcept { (void)charge; }

    // Declining by default: one venue publishes greeks today, and an adapter
    // for a venue that lists no options has nothing to say here.
    // The venue's 24-hour turnover, where it publishes one. Declining by
    // default like the rest of the optional feeds: an adapter that never calls
    // this and a market that never traded are different facts, and `caps
    // .ticker_stats` is what separates them for a caller.
    virtual void on_ticker_stats(const TickerStats& stats) noexcept { (void)stats; }

    virtual void on_option_greeks(const OptionGreeks& greeks) noexcept { (void)greeks; }

    virtual void on_order_event(const OrderEvent& event) noexcept = 0;

    // A REFUSAL WITHOUT ITS REASON IS UNDIAGNOSABLE. `OrderEvent` carries no
    // text — it is the 192-byte shm payload — so the venue's own verdict comes
    // through here, paired to the terminal event by client order id. Declining
    // by default, because a sink that only books state has nowhere to put it;
    // an adapter that was handed a reason must never drop it.
    virtual void on_order_error(ClientOrderId client_order_id, const VenueError& error) noexcept {
        (void)client_order_id;
        (void)error;
    }

    virtual void on_connectivity(const ConnEvent& event) noexcept = 0;

protected:
    ~GatewaySink() = default; // interface: never owned through the base
};

} // namespace raikiri::exec
