#include <raikiri/exec/events.hpp>
#include <raikiri/venues/weex/errors.hpp>

#include <memory>
#include <utility>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::weex::detail {

namespace {

// EVERY COMMAND IS REFUSED, LOUDLY AND LOCALLY.
//
// This adapter is public market data. There are no WEEX credentials on this
// machine, so no signing scheme has been exercised against the venue and no
// order path here has ever been answered by it — and an order path nobody has
// run is worse than none at all, because it looks like one.
//
// The client still EXISTS because the gateway dereferences it unconditionally,
// and because silence is the one answer an order may never get: a command that
// vanishes sits open in the store forever, indistinguishable from one the venue
// is still thinking about. So each verb answers immediately with the terminal
// event for its kind, and the reason travels beside it.
//
// The reconciliation pulls refuse for the same reason and the caps row declares
// each of them false, so a caller consults the row and never reads "no open
// orders" off a venue that cannot be asked.
class WeexExecution final : public exec::ExecutionClient {
public:
    explicit WeexExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    void submit(const OrderCmd& cmd) noexcept override { refuse(cmd, OrderEventType::Rejected); }

    void cancel(const OrderCmd& cmd) noexcept override {
        refuse(cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        refuse(cmd, OrderEventType::ModifyRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        return std::unexpected{exec::not_implemented("weex: order entry is not implemented")};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId&, MarketType) override {
        return std::unexpected{exec::not_implemented("weex: order entry is not implemented")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos) override {
        return std::unexpected{exec::not_implemented("weex: order entry is not implemented")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{exec::not_implemented("weex: order entry is not implemented")};
    }

private:
    void refuse(const OrderCmd& cmd, OrderEventType type) noexcept {
        const auto error =
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "weex: market data only");
        state_->sink->on_order_error(cmd.client_order_id, error);
        OrderEvent event{};
        event.type = type;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        // A create that never reached the book leaves nothing behind; a refused
        // cancel or amend leaves the order exactly as it was.
        event.leaves_qty = type == OrderEventType::Rejected ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<WeexExecution>(std::move(state));
}

} // namespace raikiri::venues::weex::detail
