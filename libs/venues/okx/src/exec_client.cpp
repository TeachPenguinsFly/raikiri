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

namespace raikiri::venues::okx::detail {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

// Where the socket meets the REST call.
//
// An order goes OUT over REST and comes BACK on the private socket, and
// nothing on either side can tell whether the other happened. This class is
// the only thing that knows both: it holds the table of orders handed over and
// not yet answered, ages them out, and synthesises the gap event that stops an
// unanswered order sitting open forever in the strategy's book.
class OkxExec final : public exec::ExecutionClient {
public:
    explicit OkxExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (!state_->credentialed()) {
            // Market data still runs; every order path below refuses, so
            // nothing can sit open waiting for a verdict no code will produce.
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
        // Whatever the builder would not take — an instrument the catalogue
        // does not know, a wedged queue — is refused out loud. A cancel that
        // vanishes leaves an order live that the strategy believes is gone.
        for (const auto& cmd : cmds.subspan(sent)) {
            emit(cmd,
                 OrderEventType::CancelRejected,
                 exec::VenueError::make(
                     exec::ErrorClass::Terminal, 0, "batch would not take this cancel"));
        }
    }

    // Forwarded, not reimplemented. These are reconciliation reads and belong
    // to the REST api; ExecutionClient names them because the gateway's
    // pollers reach a venue through one handle.
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
    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — and closed by reconciliation.
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
            // UNDEF, not zero: zero leaves would read as "nothing is working".
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
            // A CONNECTIVITY REFUSAL IS NOT THE VENUE'S ANSWER. The request may
            // have reached the matcher before the read failed, so it stays in
            // the table for the sweep rather than being called rejected here.
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
            // Never left this process, so nothing is in doubt.
            emit(cmd,
                 on_failure,
                 exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // ON THE WIRE AND UNANSWERED, recorded before the callback that may
        // never run.
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
                // WORSE IN A BATCH THAN ALONE. A lost connection here would
                // declare every cancel in the chunk refused, while the venue may
                // have taken all of them — leaving the desk sure it still has
                // orders it no longer has.
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

    // OKX answers a refused single order with envelope code "1" — "Operation
    // failed" — and puts the reason in data[].sCode. Reading only the
    // envelope turns every rejection into the same unclassifiable failure,
    // and reading only sCode misses a request that never reached the matcher.
    //
    // ABSENT MEANS ACCEPTED. The refusal carries its own reason rather than a
    // bare verdict, because sCode is the only place the reason exists and a
    // caller that got back an enum has already lost it.
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
        static thread_local AckBuffer storage;
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

    // The reason is not optional. `OrderEvent` has no room for text, so every
    // refusal hands the sink the verdict that caused it — otherwise an order
    // that dies here is indistinguishable from one that was never sent.
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

} // namespace raikiri::venues::okx::detail
