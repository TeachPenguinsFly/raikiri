// An execution client that refuses, because this venue's order path needs a
// signer this codebase does not have.
//
// THE REFUSAL IS THE FEATURE. `VenueAdapter::execution` is dereferenced
// unconditionally — gatewayd calls `execution->start()` before it knows
// whether anything will trade — so a null here is a crash at startup on a
// gateway that was only ever asked for market data. Every method below
// answers, and answers the same way: this venue authorises with an EVM WALLET
// SIGNATURE rather than the key/secret pair every other adapter here carries,
// so there is no credential that would make these paths work. The bits in the
// caps row say the same thing to anyone who asks BEFORE calling.
//
// Refusals are Terminal, never Retryable: a caller that retries a missing
// signer retries forever.

#include <raikiri/exec/execution_client.hpp>

#include <memory>
#include <string_view>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::aster::internal {

namespace {

constexpr std::string_view k_why = "aster order entry needs a wallet signer, which is not built";

[[nodiscard]] exec::VenueError refusal() noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, k_why);
}

class AsterExecution final : public exec::ExecutionClient {
public:
    explicit AsterExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    // Not an error: a market-data gateway starts this client and must come up.
    // What it must NOT do is report itself connected to an order path that
    // does not exist, which is why `connected()` below stays false.
    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    // The command reached a venue that cannot send it. Telling the caller is
    // the whole job — an order silently dropped is indistinguishable from one
    // resting on the book.
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
        // A create that never reached the book leaves nothing behind; a
        // refused cancel or amend leaves the order exactly as it was — and
        // here nothing ever reached the book, so both are zero.
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<AsterExecution>(std::move(state));
}

} // namespace raikiri::venues::aster::internal
