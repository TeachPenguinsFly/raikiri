#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/rest.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"

namespace raikiri::venues::weex::internal {

namespace {

// no private socket: an order's only answer is its rest response, and fills arrive by reconciliation
class WeexExecution final : public exec::ExecutionClient {
public:
    explicit WeexExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        api_.stop();
        started_ = false;
    }

    // connected only with the rest pool up and a credential to sign with
    [[nodiscard]] bool connected() const noexcept override { return started_ && api_.signed_path_available(); }

    // sweeps rest connections the venue closed
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    // weex has no modify for a resting order; refuse at once
    void amend(const OrderCmd& cmd) noexcept override {
        emit_reject(cmd.client_order_id,
                    cmd.instrument,
                    cmd.side,
                    cmd.qty,
                    Kind::Amend,
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, "weex has no amend; cancel and replace"));
    }

    // chunked to weex's batch limit of ten
    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        for (std::size_t at = 0; at < cmds.size(); at += k_max_cancel_batch) {
            const auto count = std::min(k_max_cancel_batch, cmds.size() - at);
            fire_batch(cmds.subspan(at, count));
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        return api_.open_order_reports(market);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) override {
        return api_.query_order(id, market);
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) override {
        return api_.fill_reports(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // an unanswered order is reported as a gap; reconciliation closes it
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : in_flight_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no answer from the venue; outcome unknown"));
            OrderEvent event{};
            event.type = OrderEventType::ReconGap;
            event.instrument = stale.instrument;
            event.client_order_id = stale.client_order_id;
            event.exchange_client_order_id = stale.exchange_client_order_id;
            event.side = stale.side;
            event.qty = stale.qty;
            // undef: whether anything is working is unknown
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request = kind == Kind::Create ? api_.builder().create_order(cmd, state_->instruments, now)
                                                  : api_.builder().cancel_order(cmd, now);
        if (!request.has_value()) {
            reject(cmd, kind, request.error());
            return;
        }
        // creates outside the api-tradable set are refused locally (-1058); cancels are never checked
        if (kind == Kind::Create && !state_->orderable(state_->instruments.exchange_symbol(cmd.instrument))) {
            emit_reject(cmd.client_order_id,
                        cmd.instrument,
                        cmd.side,
                        cmd.qty,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Terminal,
                                               k_symbol_not_api_tradable,
                                               "weex accepts no API order for this contract"));
            return;
        }
        // a placement costs no ip weight; weex meters it per account
        if (const auto allowed = kind == Kind::Create ? std::expected<void, exec::VenueError>{}
                                                      : api_.spend(ep::k_order, exec::RateClass::Order);
            !allowed.has_value()) {
            emit_reject(cmd.client_order_id, cmd.instrument, cmd.side, cmd.qty, kind, allowed.error());
            return;
        }
        const auto copy = cmd;
        if (!api_.send(*request, [this, copy, kind](net::RestResponse&& response) {
                on_response(std::move(response), copy, kind);
            })) {
            emit_reject(cmd.client_order_id,
                        cmd.instrument,
                        cmd.side,
                        cmd.qty,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Terminal, 0, "rest queue full"));
            return;
        }
        // recorded before the send, since the callback may never run
        (void)in_flight_.add({.client_order_id = cmd.client_order_id,
                              .exchange_client_order_id = cmd.exchange_client_order_id,
                              .instrument = cmd.instrument,
                              .side = cmd.side,
                              .qty = cmd.qty,
                              .kind = cmd.kind,
                              .sent = state_->clock->mono()});
    }

    void fire_batch(std::span<const OrderCmd> cmds) noexcept {
        const auto request = api_.builder().cancel_batch(cmds, state_->venue_now());
        if (!request.has_value()) {
            for (const auto& cmd : cmds) {
                reject(cmd, Kind::Cancel, request.error());
            }
            return;
        }
        if (const auto allowed = api_.spend(ep::k_batch_orders, exec::RateClass::Order); !allowed.has_value()) {
            for (const auto& cmd : cmds) {
                emit_reject(cmd.client_order_id, cmd.instrument, cmd.side, cmd.qty, Kind::Cancel, allowed.error());
            }
            return;
        }
        // copied: the caller's span is reused before the answer arrives
        std::vector<OrderCmd> batch{cmds.begin(), cmds.end()};
        if (!api_.send(*request, [this, batch](net::RestResponse&& response) {
                on_batch_response(std::move(response), batch);
            })) {
            for (const auto& cmd : cmds) {
                emit_reject(cmd.client_order_id,
                            cmd.instrument,
                            cmd.side,
                            cmd.qty,
                            Kind::Cancel,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "rest queue full"));
            }
            return;
        }
        for (const auto& cmd : cmds) {
            (void)in_flight_.add({.client_order_id = cmd.client_order_id,
                                  .exchange_client_order_id = cmd.exchange_client_order_id,
                                  .instrument = cmd.instrument,
                                  .side = cmd.side,
                                  .qty = cmd.qty,
                                  .kind = cmd.kind,
                                  .sent = state_->clock->mono()});
        }
    }

    // whole-request verdict; a benign cancel is still returned since only the caller knows the verb
    [[nodiscard]] std::optional<exec::VenueError> request_failure(const net::RestResponse& response) const noexcept {
        const auto error = parse_rest_error(response.body);
        if (!error.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable answer");
        }
        if (error->code != k_ok) {
            return exec::VenueError::make(classify(error->code), error->code, error->msg);
        }
        if (response.status < 200 || response.status >= 300) {
            // an unrouted path answers with no code
            return exec::VenueError::make(exec::ErrorClass::Terminal, response.status, "venue refused without a code");
        }
        return std::nullopt;
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) noexcept {
        // any answer settles it; the table tracks silence
        if (!response.transport_failed()) {
            (void)in_flight_.resolve(cmd.client_order_id);
        } else {
            // the request may have reached the matcher; the sweep reports it as a gap
            state_->sink->on_order_error(cmd.client_order_id,
                                         exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                0,
                                                                response.error.empty()
                                                                    ? std::string_view{"no response from venue"}
                                                                    : std::string_view{response.error}));
            return;
        }

        const auto failure = request_failure(response);
        if (failure.has_value()) {
            if (kind == Kind::Cancel && is_benign_cancel_failure(failure->venue_code)) {
                // the cancel arrived late; the order is already closed
                emit(cmd, OrderEventType::Canceled, {}, QtyLots::from_raw(0));
                return;
            }
            emit_reject(cmd.client_order_id, cmd.instrument, cmd.side, cmd.qty, kind, *failure);
            return;
        }
        const auto ack = parse_order_ack(response.body);
        if (!ack.has_value()) {
            emit_reject(cmd.client_order_id,
                        cmd.instrument,
                        cmd.side,
                        cmd.qty,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable order acknowledgement"));
            return;
        }
        publish(cmd, kind, *ack);
    }

    void on_batch_response(net::RestResponse&& response, const std::vector<OrderCmd>& batch) noexcept {
        for (const auto& cmd : batch) {
            if (!response.transport_failed()) {
                (void)in_flight_.resolve(cmd.client_order_id);
            }
        }
        if (response.transport_failed()) {
            for (const auto& cmd : batch) {
                state_->sink->on_order_error(cmd.client_order_id,
                                             exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                    0,
                                                                    response.error.empty()
                                                                        ? std::string_view{"no response from venue"}
                                                                        : std::string_view{response.error}));
            }
            return;
        }
        const auto failure = request_failure(response);
        if (failure.has_value()) {
            const bool already_gone = is_benign_cancel_failure(failure->venue_code);
            for (const auto& cmd : batch) {
                if (already_gone) {
                    emit(cmd, OrderEventType::Canceled, {}, QtyLots::from_raw(0));
                    continue;
                }
                emit_reject(cmd.client_order_id, cmd.instrument, cmd.side, cmd.qty, Kind::Cancel, *failure);
            }
            return;
        }
        static thread_local const auto storage_owner = std::make_unique<OrderAckBuffer>();
        auto& storage = *storage_owner;
        const auto acks = parse_order_acks(response.body, storage);
        if (!acks.has_value()) {
            for (const auto& cmd : batch) {
                emit_reject(cmd.client_order_id,
                            cmd.instrument,
                            cmd.side,
                            cmd.qty,
                            Kind::Cancel,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable batch acknowledgement"));
            }
            return;
        }
        // matched by client id since weex states no ordering; an unnamed command stays in doubt
        for (const auto& cmd : batch) {
            for (const auto& ack : *acks) {
                if (ack.client_order_id == cmd.exchange_client_order_id.view()) {
                    publish(cmd, Kind::Cancel, ack);
                    break;
                }
            }
        }
    }

    void publish(const OrderCmd& cmd, Kind kind, const WireOrderAck& ack) noexcept {
        if (!ack.success) {
            if (kind == Kind::Cancel && is_benign_cancel_error(ack.error_code)) {
                // the cancel arrived late; the order is already closed
                emit(cmd, OrderEventType::Canceled, ack.order_id, QtyLots::from_raw(0));
                return;
            }
            const auto reason = ack.error_message.empty() ? ack.error_code : ack.error_message;
            emit_reject(cmd.client_order_id,
                        cmd.instrument,
                        cmd.side,
                        cmd.qty,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Terminal, 0, reason));
            return;
        }
        if (kind == Kind::Cancel) {
            emit(cmd, OrderEventType::Canceled, ack.order_id, QtyLots::from_raw(0));
            return;
        }
        // the place answer says nothing of fills, so leaves is the whole order
        emit(cmd, OrderEventType::Accepted, ack.order_id, cmd.qty);
    }

    void emit(const OrderCmd& cmd, OrderEventType type, std::string_view order_id, QtyLots leaves) noexcept {
        OrderEvent event{};
        populate(event, cmd);
        event.type = type;
        if (const auto id = ExchangeOrderId::from(order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.leaves_qty = leaves;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    void populate(OrderEvent& event, const OrderCmd& cmd) const noexcept {
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
    }

    // every refusal reaches the sink with its verdict
    void emit_reject(ClientOrderId client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind,
                     const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.instrument = instrument;
        event.side = side;
        event.client_order_id = client_order_id;
        event.qty = qty;
        // a rejected create leaves nothing; a refused cancel or amend leaves the order as it was
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    void reject(const OrderCmd& cmd, Kind kind, BuildError why) noexcept {
        // unsigned means an auth failure, which halts the gateway
        const auto cls = why == BuildError::NotSigned ? exec::ErrorClass::AuthFailure : exec::ErrorClass::Terminal;
        emit_reject(cmd.client_order_id,
                    cmd.instrument,
                    cmd.side,
                    cmd.qty,
                    kind,
                    exec::VenueError::make(cls, 0, to_string(why)));
    }

    std::shared_ptr<VenueState> state_;
    RestApi api_;
    exec::InFlightTable in_flight_{k_max_unanswered};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<WeexExecution>(std::move(state));
}

} // namespace raikiri::venues::weex::internal
