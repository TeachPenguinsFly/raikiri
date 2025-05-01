#pragma once

// Execution side of a venue adapter.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
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
    std::uint8_t liquidity{0}; // 0 unknown, 1 maker, 2 taker
    UnixNanos ts_venue{};
};

struct PositionReport {
    InstrumentId instrument{};
    QtyLots net_qty{}; // signed: positive long, negative short
    PriceTicks avg_entry{};
    UnixNanos ts_venue{};
};

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

    // Native price/qty modify.
    virtual void amend(const OrderCmd& cmd) noexcept { (void)cmd; }

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
};

} // namespace raikiri::exec
