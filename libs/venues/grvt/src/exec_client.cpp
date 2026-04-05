// An execution client that refuses, because this venue's order path needs a
// signer this codebase does not have.
//
// THE REFUSAL IS THE FEATURE. `VenueAdapter::execution` is dereferenced
// unconditionally — gatewayd calls `execution->start()` before it knows
// whether anything will trade — so a null here is a crash at startup on a
// gateway that was only ever asked for market data. GRVT authorises every
// order with an EIP-712 TYPED-DATA SIGNATURE from a wallet key over the order
// itself — not an HMAC over the request — so there is no key/secret pair that
// would make these paths work. The caps row says the same to anyone who asks
// before calling.
//
// Refusals are Terminal, never Retryable: a caller that retries a missing
// signer retries forever.

#include <raikiri/exec/execution_client.hpp>

#include <memory>
#include <string_view>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::grvt::detail {

namespace {

constexpr std::string_view k_why =
    "grvt order entry needs an EIP-712 wallet signer, which is not built";

[[nodiscard]] exec::VenueError refusal() noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, k_why);
}

class GrvtExecution final : public exec::ExecutionClient {
public:
    explicit GrvtExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    // A market-data gateway starts this client and must come up; `connected()`
    // stays false because there is no order path to be connected to.
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
    // The reason rides on `on_order_error`, because the terminal event has no
    // room for one and a rejection nobody can explain costs an hour.
    void emit_reject(const OrderCmd& cmd, OrderEventType type) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, refusal());
        OrderEvent event{};
        event.type = type;
        event.instrument = cmd.instrument;
        event.side = cmd.side;
        event.client_order_id = cmd.client_order_id;
        event.qty = cmd.qty;
        // Nothing ever reached the book, so nothing is left on it.
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<GrvtExecution>(std::move(state));
}

} // namespace raikiri::venues::grvt::detail
