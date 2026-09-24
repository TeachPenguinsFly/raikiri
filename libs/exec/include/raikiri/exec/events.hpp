#pragma once

// the event boundary between a venue adapter and the gateway core; wait-free, never throws

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

    // asked before building a snapshot; the shm cell holds only the latest value
    [[nodiscard]] virtual bool wants_depth(InstrumentId instrument, UnixNanos now) noexcept {
        (void)instrument;
        (void)now;
        return true;
    }

    virtual void on_depth(const BookSnapshot& book) noexcept { (void)book; }

    virtual void on_funding(const FundingUpdate& funding) noexcept = 0;

    // what the venue charged, as opposed to the rate; paired with caps.funding_charge
    virtual void on_funding_charge(const LedgerEntry& charge) noexcept { (void)charge; }

    // the venue's 24 h turnover; caps.ticker_stats tells silence from no trades
    virtual void on_ticker_stats(const TickerStats& stats) noexcept { (void)stats; }

    virtual void on_option_greeks(const OptionGreeks& greeks) noexcept { (void)greeks; }

    virtual void on_order_event(const OrderEvent& event) noexcept = 0;

    // the venue's reason for a refusal, paired to the terminal event by client order id
    virtual void on_order_error(ClientOrderId client_order_id, const VenueError& error) noexcept {
        (void)client_order_id;
        (void)error;
    }

    virtual void on_connectivity(const ConnEvent& event) noexcept = 0;

protected:
    ~GatewaySink() = default; // interface: never owned through the base
};

} // namespace raikiri::exec
