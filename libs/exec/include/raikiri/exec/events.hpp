#pragma once

// The single event boundary between a venue adapter and the gateway core.
//
// Design decision (vs. the Go predecessor): the Go layer exposed one
// channel-returning interface PER data type (BBOUpdates, TradeUpdates,
// KlineUpdates, FundingRateUpdates, OrderBookUpdates) plus a separate order
// result return AND a separate status publisher. Three consequences we
// refuse to inherit: type-erased plumbing per stream, allocation per
// message, and order outcomes split across two paths. Here every adapter
// pushes typed, fixed-point, symbol-interned PODs into ONE sink, and every
// order outcome — venue ack, fill, and locally-synthesized transport
// rejects alike — is an OrderEvent. One boundary, one vocabulary.
//
// Hot-path contract: implementations are wait-free (they write seqlock
// tables / broadcast rings / journal appends) and never throw; adapters may
// call from their I/O thread at full tick rate.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>

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
    virtual void on_funding(const FundingUpdate& funding) noexcept = 0;
    virtual void on_order_event(const OrderEvent& event) noexcept = 0;
    virtual void on_connectivity(const ConnEvent& event) noexcept = 0;

protected:
    ~GatewaySink() = default; // interface: never owned through the base
};

} // namespace raikiri::exec
