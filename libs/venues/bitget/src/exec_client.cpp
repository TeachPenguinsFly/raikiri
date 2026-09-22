#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/orders.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::bitget::internal {

namespace {

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Where the socket meets the REST call.
//
// An order goes OUT over REST and comes BACK on the private socket, and
// nothing on either side can tell whether the other happened. This class is
// the only thing that knows both: it holds the table of orders handed over and
// not yet answered, ages them out, and synthesises the gap event that stops an
// unanswered order sitting open forever in the strategy's book.
class BitgetExec final : public exec::ExecutionClient {
public:
    explicit BitgetExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        stream_.start();
        return {};
    }

    void stop() override {
        stream_.stop();
        api_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_.connected(); }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — because whether the venue acted is the one
    // thing this process cannot know. Reconciliation closes these.
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
            // UNDEF, not zero: zero leaves would read as "nothing is working",
            // and whether anything is working is exactly what is unknown.
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    // Bitget has no in-place amend on either API. Mix `modify-order` requires
    // a newClientOid and creates a DIFFERENT order asynchronously; spot only
    // offers `cancel-replace-order`, which does the same. An OrderCmd carries
    // one client id and no second one to give the replacement, so honouring
    // either would leave the OMS tracking an order the venue has cancelled.
    // The caps row says amend_order = false and callers cancel and replace
    // with two ids of their own — which is what Bitget does internally anyway.
    void amend(const OrderCmd& cmd) noexcept override {
        emit(cmd,
             OrderEventType::ModifyRejected,
             exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitget has no in-place amend"));
    }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        std::size_t sent = 0;
        while (sent < cmds.size()) {
            const auto remaining = cmds.subspan(sent);
            const auto batch = api_.builder().cancel_batch(
                remaining, state_->instruments, state_->product(), state_->venue_now());
            if (!batch.has_value() || batch->consumed == 0) {
                // One command the batch cannot express must not strand the
                // rest of the panic cancel behind it.
                cancel(remaining.front());
                ++sent;
                continue;
            }
            std::vector<OrderCmd> members{remaining.begin(),
                                          remaining.begin() +
                                              static_cast<std::ptrdiff_t>(batch->consumed)};
            if (!api_.send(batch->request, [this, members](net::RestResponse&& response) {
                    on_batch_response(std::move(response), members);
                })) {
                // Never left this process, so nothing is in doubt.
                for (const auto& member : members) {
                    emit(member,
                         OrderEventType::CancelRejected,
                         exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
                }
            } else {
                // ON THE WIRE AND UNANSWERED, every member of it: one lost read
                // leaves the whole batch's outcome unknown, not refused.
                for (const auto& member : members) {
                    (void)unanswered_.add(
                        {.client_order_id = member.client_order_id,
                         .exchange_client_order_id = member.exchange_client_order_id,
                         .instrument = member.instrument,
                         .side = member.side,
                         .qty = member.qty,
                         .kind = member.kind,
                         .sent = state_->clock->mono()});
                }
            }
            sent += batch->consumed;
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

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError>
    funding_charges(MarketType market, UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

private:
    // What a command is asking the venue to do. The two share one path because
    // they share one failure: an unanswered request.
    enum class Kind : std::uint8_t { Create, Cancel };

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create
                ? api_.builder().create_order(cmd, state_->instruments, state_->product(), now)
                : api_.builder().cancel_order(cmd, state_->instruments, state_->product(), now);
        if (!request.has_value()) {
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const OrderCmd snapshot = cmd; // 128 trivially-copyable bytes
        if (!api_.send(*request, [this, snapshot, kind](net::RestResponse&& response) {
                on_response(std::move(response), snapshot, kind);
            })) {
            // Never left this process, so nothing is in doubt.
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // ON THE WIRE AND UNANSWERED. Recorded here rather than in the callback
        // because the callback is exactly what may never run.
        (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                               .exchange_client_order_id = cmd.exchange_client_order_id,
                               .instrument = cmd.instrument,
                               .side = cmd.side,
                               .qty = cmd.qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    static OrderEventType refusal_of(Kind kind) noexcept {
        // CancelRejected, not Rejected: the order a cancel targets is alive.
        return kind == Kind::Create ? OrderEventType::Rejected : OrderEventType::CancelRejected;
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        // Any answer settles it, including a refusal: the table tracks silence.
        if (!response.transport_failed()) {
            (void)unanswered_.resolve(cmd.client_order_id);
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The request may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position.
            state_->sink->on_order_error(
                cmd.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       response.error.empty()
                                           ? std::string_view{"no response from venue"}
                                           : std::string_view{response.error}));
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code == k_ok) {
            // The acceptance itself arrives on the private socket; the REST
            // answer carries only ids, and an id without a lifecycle event is
            // a second source of truth nobody reads.
            return;
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->code)) {
            return;
        }
        emit(cmd,
             refusal_of(kind),
             exec::VenueError::make(
                 classify(envelope->code), code_number(envelope->code), envelope->msg));
    }

    void on_batch_response(net::RestResponse&& response, const std::vector<OrderCmd>& members) {
        const auto reject_all = [&](const exec::VenueError& error) {
            for (const auto& member : members) {
                emit(member, OrderEventType::CancelRejected, error);
            }
        };
        // Any answer settles it, including a refusal: the table tracks silence.
        if (!response.transport_failed()) {
            for (const auto& member : members) {
                (void)unanswered_.resolve(member.client_order_id);
            }
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The batch may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position.
            const auto error = exec::VenueError::make(
                exec::ErrorClass::Connectivity,
                0,
                response.error.empty() ? std::string_view{"no response from venue"}
                                       : std::string_view{response.error});
            for (const auto& member : members) {
                state_->sink->on_order_error(member.client_order_id, error);
            }
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            reject_all(
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code != k_ok) {
            if (!is_benign_cancel_failure(envelope->code)) {
                reject_all(exec::VenueError::make(
                    classify(envelope->code), code_number(envelope->code), envelope->msg));
            }
            return;
        }
        // A batch answers 00000 while refusing individual orders inside
        // failureList, so a caller that reads only the envelope believes it
        // cancelled orders that are still working.
        auto storage = std::make_unique<BatchFailureBuffer>();
        const auto failures = parse_batch_failures(response.body, *storage);
        if (!failures.has_value()) {
            return;
        }
        for (const auto& failure : *failures) {
            if (is_benign_cancel_failure(failure.code)) {
                continue;
            }
            for (const auto& member : members) {
                if (member.exchange_client_order_id.view() == failure.client_oid) {
                    emit(member,
                         OrderEventType::CancelRejected,
                         exec::VenueError::make(
                             classify(failure.code), code_number(failure.code), failure.msg));
                }
            }
        }
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
        // A rejected create leaves nothing working; a refused cancel or modify
        // leaves the order exactly as it was.
        event.leaves_qty = type == OrderEventType::Rejected ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    PrivateStream stream_;
    RestApi api_;
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitgetExec>(std::move(state));
}

} // namespace raikiri::venues::bitget::internal
