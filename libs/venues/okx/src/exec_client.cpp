#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/orders.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::okx::internal {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

// orders go out over rest and come back on the private socket; this class ages out unanswered ones
class OkxExec final : public exec::ExecutionClient {
public:
    explicit OkxExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (!state_->credentialed()) {
            return {};
        }
        stream_.start();
        return {};
    }

    void stop() override {
        stream_.stop();
        api_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_.connected(); }

    void submit(const OrderCmd& cmd) noexcept override {
        fire(api_.builder().create_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        fire(api_.builder().cancel_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        fire(api_.builder().amend_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::ModifyRejected);
    }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        std::size_t sent = 0;
        while (sent < cmds.size()) {
            const auto batch = api_.builder().cancel_batch(
                cmds.subspan(sent), state_->instruments, state_->venue_now());
            if (!batch.has_value() || batch->consumed == 0) {
                break;
            }
            const auto chunk = cmds.subspan(sent, batch->consumed);
            if (!send_batch(batch->request, chunk)) {
                break;
            }
            sent += batch->consumed;
        }
        // anything the builder would not take is refused explicitly
        for (const auto& cmd : cmds.subspan(sent)) {
            emit(cmd,
                 OrderEventType::CancelRejected,
                 exec::VenueError::make(
                     exec::ErrorClass::Terminal, 0, "batch would not take this cancel"));
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        return api_.open_order_reports(market);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        return api_.query_order(id, market);
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        return api_.fill_reports(market, since);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError>
    funding_charges(MarketType market, UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

private:
    // sweeps idle rest connections the venue already closed
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    // an unanswered order is reported as a gap; reconciliation closes it
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale :
             unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       "no answer from the venue; outcome unknown"));
            OrderEvent event{};
            event.type = OrderEventType::ReconGap;
            event.instrument = stale.instrument;
            event.client_order_id = stale.client_order_id;
            event.exchange_client_order_id = stale.exchange_client_order_id;
            event.side = stale.side;
            event.qty = stale.qty;
            // undef: zero leaves would read as nothing working
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void fire(const std::expected<net::RestRequest, BuildError>& request,
              const OrderCmd& cmd,
              OrderEventType on_failure) noexcept {
        if (!request.has_value()) {
            emit(cmd,
                 on_failure,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const auto captured = cmd;
        const auto sent = api_.send(*request, [this, captured, on_failure](net::RestResponse&& r) {
            const auto refusal = verdict(r, captured.exchange_client_order_id, on_failure);
            // the request may have reached the matcher, so it stays for the sweep
            if (refusal.has_value() && refusal->cls == exec::ErrorClass::Connectivity) {
                state_->sink->on_order_error(captured.client_order_id, *refusal);
                return;
            }
            (void)unanswered_.resolve(captured.client_order_id);
            if (refusal.has_value()) {
                emit(captured, on_failure, *refusal);
            }
        });
        if (!sent) {
            emit(cmd,
                 on_failure,
                 exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                               .exchange_client_order_id = cmd.exchange_client_order_id,
                               .instrument = cmd.instrument,
                               .side = cmd.side,
                               .qty = cmd.qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    bool send_batch(const net::RestRequest& request, std::span<const OrderCmd> chunk) noexcept {
        std::vector<OrderCmd> captured{chunk.begin(), chunk.end()};
        const bool sent = api_.send(request, [this, captured](net::RestResponse&& r) {
            for (const auto& cmd : captured) {
                const auto refusal =
                    verdict(r, cmd.exchange_client_order_id, OrderEventType::CancelRejected);
                // the venue may have taken every cancel in the chunk
                if (refusal.has_value() && refusal->cls == exec::ErrorClass::Connectivity) {
                    state_->sink->on_order_error(cmd.client_order_id, *refusal);
                    continue;
                }
                (void)unanswered_.resolve(cmd.client_order_id);
                if (refusal.has_value()) {
                    emit(cmd, OrderEventType::CancelRejected, *refusal);
                }
            }
        });
        if (!sent) {
            return false;
        }
        for (const auto& cmd : chunk) {
            (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                                   .exchange_client_order_id = cmd.exchange_client_order_id,
                                   .instrument = cmd.instrument,
                                   .side = cmd.side,
                                   .qty = cmd.qty,
                                   .kind = cmd.kind,
                                   .sent = state_->clock->mono()});
        }
        return true;
    }

    // a refused order answers envelope code 1 with the reason in data[].sCode; absent means accepted
    [[nodiscard]] std::optional<exec::VenueError> verdict(const net::RestResponse& response,
                                                          const ExchangeClientOrderId& id,
                                                          OrderEventType on_failure) const {
        if (response.transport_failed()) {
            return exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "no response from venue");
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope");
        }
        static thread_local const auto storage_owner = std::make_unique<AckBuffer>();
        auto& storage = *storage_owner;
        const auto acks = parse_order_acks(response.body, storage);
        std::int64_t code = envelope->code;
        std::string_view text = envelope->msg;
        if (acks.has_value()) {
            for (const auto& ack : *acks) {
                // A batch answers for several orders at once, so the entry
                // that matters is the one carrying this order's id.
                if (ack.cl_ord_id == id.view() || acks->size() == 1) {
                    code = ack.s_code;
                    text = ack.s_msg;
                    break;
                }
            }
        }
        if (code == k_ok) {
            return std::nullopt; // the private stream reports what follows
        }
        if (on_failure == OrderEventType::CancelRejected && is_benign_cancel_failure(code)) {
            return std::nullopt; // the order already filled or was gone
        }
        if (on_failure == OrderEventType::ModifyRejected && is_benign_amend_failure(code)) {
            return std::nullopt;
        }
        return exec::VenueError::make(classify(code), code, text);
    }

    void emit(const OrderCmd& cmd, OrderEventType type, const exec::VenueError& error) noexcept {
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
        event.leaves_qty = type == OrderEventType::Rejected ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    PrivateStream stream_;
    RestApi api_;
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<OkxExec>(std::move(state));
}

} // namespace raikiri::venues::okx::internal
