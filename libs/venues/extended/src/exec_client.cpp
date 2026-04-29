// gatewayd starts this client unconditionally, so it must exist. Every call is
// refused Terminal: an order needs a Stark-key signature over its settlement,
// and an RFQ market its own order path; no signer for either exists.

#include <raikiri/exec/execution_client.hpp>

#include <memory>
#include <string_view>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::extended::detail {

namespace {

constexpr std::string_view k_why =
    "extended order entry needs a Stark-key settlement signer, which is not built";

[[nodiscard]] exec::VenueError refusal() noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, k_why);
}

class ExtendedExecution final : public exec::ExecutionClient {
public:
    explicit ExtendedExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    void submit(const OrderCmd& cmd) noexcept override {
        emit_reject(cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        emit_reject(cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        emit_reject(cmd, OrderEventType::ModifyRejected);
    }

    [[nodiscard]] std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId&, MarketType) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{refusal()};
    }

private:
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
    return std::make_unique<ExtendedExecution>(std::move(state));
}

} // namespace raikiri::venues::extended::detail
