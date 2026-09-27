#include <raikiri/exec/execution_client.hpp>

#include <memory>
#include <string_view>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::lbank::internal {

namespace {

constexpr std::string_view k_no_credentials = "lbank: no credentials configured";
constexpr std::string_view k_no_perp_api = "lbank: the contract api publishes no order entry, only public market data";
constexpr std::string_view k_no_spot_exec = "lbank: spot order entry is not built";

class LbankExecution final : public exec::ExecutionClient {
public:
    explicit LbankExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    [[nodiscard]] exec::VenueError refusal() const noexcept {
        const auto why = !state_->has_credentials             ? k_no_credentials
                         : state_->market == MarketType::Perp ? k_no_perp_api
                                                              : k_no_spot_exec;
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, why);
    }

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
    return std::make_unique<LbankExecution>(std::move(state));
}

} // namespace raikiri::venues::lbank::internal
