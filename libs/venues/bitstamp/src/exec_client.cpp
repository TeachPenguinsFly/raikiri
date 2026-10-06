// refuses every order; execution is dereferenced unconditionally, so this client must exist

#include <raikiri/exec/execution_client.hpp>

#include <memory>
#include <string_view>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::bitstamp::internal {

namespace {

constexpr std::string_view k_why = "order entry not implemented: market data only";

[[nodiscard]] exec::VenueError refusal() noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, k_why);
}

class RefusingExecution final : public exec::ExecutionClient {
public:
    explicit RefusingExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    // a market-data gateway starts this client; connected() stays false
    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    void submit(const OrderCmd& cmd) noexcept override { emit_reject(cmd, OrderEventType::Rejected); }

    void cancel(const OrderCmd& cmd) noexcept override { emit_reject(cmd, OrderEventType::CancelRejected); }

    void amend(const OrderCmd& cmd) noexcept override { emit_reject(cmd, OrderEventType::ModifyRejected); }

    [[nodiscard]] std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId&,
                                                                                       MarketType) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType,
                                                                                              UnixNanos) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{refusal()};
    }

private:
    // the reason goes on on_order_error; the terminal event has no field for it
    void emit_reject(const OrderCmd& cmd, OrderEventType type) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, refusal());
        OrderEvent event{};
        event.type = type;
        event.instrument = cmd.instrument;
        event.side = cmd.side;
        event.client_order_id = cmd.client_order_id;
        event.qty = cmd.qty;
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<RefusingExecution>(std::move(state));
}

} // namespace raikiri::venues::bitstamp::internal
