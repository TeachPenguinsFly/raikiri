#pragma once

// Execution side of a venue adapter.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/domain/position_report.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/error.hpp>

#include <expected>
#include <span>
#include <vector>

namespace raikiri::exec {

struct OrderStatusReport {
    ExchangeClientOrderId exchange_client_order_id{};
    ExchangeOrderId exchange_order_id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    OrderStatus status{OrderStatus::Accepted};
    PriceTicks price{};
    QtyLots qty{};
    QtyLots leaves{};
    UnixNanos ts_venue{};
};

struct FillReport {
    ExecId exec_id{};
    ExchangeOrderId exchange_order_id{};
    ExchangeClientOrderId exchange_client_order_id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    PriceTicks price{};
    QtyLots qty{};
    FixedE9 fee{};
    Liquidity liquidity{Liquidity::Unknown};
    UnixNanos ts_venue{};
};

// Lives in domain: the shm transport carries it too, and transport cannot
// depend on this layer. Spelled exec::PositionReport everywhere it is used.
using PositionReport = raikiri::PositionReport;

// An adapter that does not implement a pull must SAY SO. An empty vector is a
// legitimate answer meaning "flat", and a reconciler cannot tell the two apart:
// read the wrong way it concludes the venue reports no position while we hold
// one, and acts on it.
[[nodiscard]] VenueError not_implemented(std::string_view what) noexcept;

class ExecutionClient {
public:
    ExecutionClient() = default;
    ExecutionClient(const ExecutionClient&) = delete;
    ExecutionClient& operator=(const ExecutionClient&) = delete;
    virtual ~ExecutionClient() = default;

    // Cold lifecycle: user-stream connect/auth, warm REST keep-alives.
    virtual std::expected<void, VenueError> start() = 0;
    virtual void stop() = 0; // must be idempotent (teardown calls it twice)
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    virtual void submit(const OrderCmd& cmd) noexcept = 0;
    virtual void cancel(const OrderCmd& cmd) noexcept = 0;

    virtual void amend(const OrderCmd& cmd) noexcept { (void)cmd; }

    // Driven every loop by the gateway. WITHOUT THIS THE ORDER PATH ROTS WHILE
    // IDLE: the REST pool holds a connection open, the venue closes its end
    // after its own idle timeout, and the next order is written into a socket
    // that is already gone. That failure is worse than a refusal — the request
    // can reach the matcher before the read fails, so the venue fills an order
    // this process has recorded as rejected.
    virtual void poll(UnixNanos now) noexcept { (void)now; }

    virtual void cancel_batch(std::span<const OrderCmd> cmds) noexcept {
        for (const auto& cmd : cmds) {
            cancel(cmd);
        }
    }

    // Cold reconciliation pulls (REST). Called on start and re-attach.
    [[nodiscard]] virtual std::expected<std::vector<OrderStatusReport>, VenueError>
    open_order_reports(MarketType market) = 0;

    [[nodiscard]] virtual std::expected<OrderStatusReport, VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) = 0;

    [[nodiscard]] virtual std::expected<std::vector<FillReport>, VenueError>
    fill_reports(MarketType market, UnixNanos since) = 0;

    [[nodiscard]] virtual std::expected<std::vector<PositionReport>, VenueError>
    position_reports(MarketType market) = 0;

    // What the venue CHARGED for funding, from its own ledger — never derived
    // from the public rate. Declining by default and gated by
    // `VenueCaps::funding_charge` for the reason position_pull exists: an
    // adapter that returns nothing because it cannot ask, and one that returns
    // nothing because nothing was charged, are different facts, and booking
    // the second when the first is true loses money silently.
    [[nodiscard]] virtual std::expected<std::vector<LedgerEntry>, VenueError>
    funding_charges(MarketType market, UnixNanos since) {
        (void)market;
        (void)since;
        return std::unexpected{
            VenueError::make(ErrorClass::Terminal, 0, "venue reports no funding charges")};
    }

    // What the account HOLDS, as opposed to what it is long or short. Defaulted
    // to a refusal rather than made pure: an adapter opts in, and `caps
    // .balance_pull` is what a caller consults BEFORE calling — without it an
    // adapter that answers "no balances" and one that cannot answer at all look
    // identical, which is the same trap position_pull exists to avoid.
    [[nodiscard]] virtual std::expected<std::vector<BalanceReport>, VenueError>
    balance_reports(MarketType market) {
        (void)market;
        return std::unexpected{
            VenueError::make(ErrorClass::Terminal, 0, "balances not implemented for this venue")};
    }
};

} // namespace raikiri::exec
