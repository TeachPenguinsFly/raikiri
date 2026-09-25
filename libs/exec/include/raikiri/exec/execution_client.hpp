#pragma once

// execution side of a venue adapter

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/fill_report.hpp>
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

using FillReport = raikiri::FillReport;

// lives in domain because the shm transport carries it
using PositionReport = raikiri::PositionReport;

// an unimplemented pull must return this; an empty vector means flat
[[nodiscard]] VenueError not_implemented(std::string_view what) noexcept;

class ExecutionClient {
public:
    ExecutionClient() = default;
    ExecutionClient(const ExecutionClient&) = delete;
    ExecutionClient& operator=(const ExecutionClient&) = delete;
    virtual ~ExecutionClient() = default;

    // cold lifecycle: user-stream connect and auth
    virtual std::expected<void, VenueError> start() = 0;
    virtual void stop() = 0; // must be idempotent (teardown calls it twice)
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    virtual void submit(const OrderCmd& cmd) noexcept = 0;
    virtual void cancel(const OrderCmd& cmd) noexcept = 0;

    virtual void amend(const OrderCmd& cmd) noexcept { (void)cmd; }

    // driven every loop so the rest pool does not write into a socket the venue closed while idle
    virtual void poll(UnixNanos now) noexcept { (void)now; }

    virtual void cancel_batch(std::span<const OrderCmd> cmds) noexcept {
        for (const auto& cmd : cmds) {
            cancel(cmd);
        }
    }

    // cold reconciliation pulls over rest, on start and re-attach
    [[nodiscard]] virtual std::expected<std::vector<OrderStatusReport>, VenueError>
    open_order_reports(MarketType market) = 0;

    [[nodiscard]] virtual std::expected<OrderStatusReport, VenueError> query_order(const ExchangeClientOrderId& id,
                                                                                   MarketType market) = 0;

    [[nodiscard]] virtual std::expected<std::vector<FillReport>, VenueError> fill_reports(MarketType market,
                                                                                          UnixNanos since) = 0;

    [[nodiscard]] virtual std::expected<std::vector<PositionReport>, VenueError>
    position_reports(MarketType market) = 0;

    // the venue's own funding ledger; gated by VenueCaps::funding_charge
    [[nodiscard]] virtual std::expected<std::vector<LedgerEntry>, VenueError> funding_charges(MarketType market,
                                                                                              UnixNanos since) {
        (void)market;
        (void)since;
        return std::unexpected{VenueError::make(ErrorClass::Terminal, 0, "venue reports no funding charges")};
    }

    // account balances; gated by caps.balance_pull for the market asked
    [[nodiscard]] virtual std::expected<std::vector<BalanceReport>, VenueError> balance_reports(MarketType market) {
        (void)market;
        return std::unexpected{VenueError::make(ErrorClass::Terminal, 0, "balances not implemented for this venue")};
    }
};

} // namespace raikiri::exec
