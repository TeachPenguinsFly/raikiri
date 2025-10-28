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

namespace raikiri::venues::weex::detail {

namespace {

// The order path, and everything it cannot know.
//
// NO CREDENTIAL HAS EVER REACHED THIS VENUE FROM THIS MACHINE. Every request
// below is built from what WEEX documents and has been answered only by its own
// 401. The caps row therefore claims none of it, and a reader deciding whether
// to trust this path should read that row rather than this comment.
//
// THERE IS NO PRIVATE SOCKET HERE, which is the shape of everything else in
// this file. WEEX publishes one — /v3/ws/private, authenticated by ACCESS-*
// headers on the handshake — and this adapter does not open it, so an order's
// only answer is its own REST response and a FILL ARRIVES AT NO EVENT AT ALL:
// fills reach this process through the reconciliation pull and nowhere else.
// An order accepted here is Accepted; what it then does is reconciliation's
// question until that socket exists.
class WeexExecution final : public exec::ExecutionClient {
public:
    explicit WeexExecution(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        api_.stop();
        started_ = false;
    }

    // No socket to be up or down, so this answers from whether the REST pool is
    // up AND a credential exists to sign with: without one there is no order
    // path at all, and reporting a connection would be a claim about a venue
    // this process cannot address.
    [[nodiscard]] bool connected() const noexcept override {
        return started_ && api_.signed_path_available();
    }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it, and an order written into
    // a dead socket can still reach the matcher before the read fails.
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    // REFUSED BY THE VENUE'S OWN SHAPE, not by this adapter's scope: WEEX
    // publishes no modify for a resting order — only a take-profit trigger can
    // be moved — so a caller cancels and replaces. Answered locally and at once,
    // because silence is the one answer an amend may never get.
    void amend(const OrderCmd& cmd) noexcept override {
        emit_reject(cmd.client_order_id,
                    cmd.instrument,
                    cmd.side,
                    cmd.qty,
                    Kind::Amend,
                    exec::VenueError::make(
                        exec::ErrorClass::Terminal, 0, "weex has no amend; cancel and replace"));
    }

    // Ten at a time, which is WEEX's own ceiling on one batch. Chunked here
    // rather than refused, because the caller's batch is a desk's book and the
    // venue's limit is an implementation detail of one request.
    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        for (std::size_t at = 0; at < cmds.size(); at += k_max_cancel_batch) {
            const auto count = std::min(k_max_cancel_batch, cmds.size() - at);
            fire_batch(cmds.subspan(at, count));
        }
    }

    // Forwarded, not reimplemented. These are reconciliation reads and belong
    // to the REST api; ExecutionClient names them because the gateway's pollers
    // reach a venue through one handle.
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

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

private:
    // What a command is asking the venue to do. They share one path because
    // they share one failure: an unanswered request.
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — because whether the venue acted is the one
    // thing this process cannot know. Reconciliation closes these.
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale :
             in_flight_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
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
            // and whether anything is working is exactly what is unknown here.
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request = kind == Kind::Create
                                 ? api_.builder().create_order(cmd, state_->instruments, now)
                                 : api_.builder().cancel_order(cmd, now);
        if (!request.has_value()) {
            reject(cmd, kind, request.error());
            return;
        }
        // REFUSED HERE RATHER THAN BY THE VENUE. WEEX lists a thousand
        // contracts and accepts orders for 290 of them; the rest answer -1058,
        // which arrives in a log looking like a defect in this adapter and
        // costs a round trip to learn nothing. A cancel is not checked — an
        // order already resting must always be cancellable, whatever the list
        // says today.
        if (kind == Kind::Create &&
            !state_->orderable(state_->instruments.exchange_symbol(cmd.instrument))) {
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
        // ORDER CLASS EITHER WAY, which may spend the reserve a poll may not —
        // but not the same bucket, because WEEX does not meter the two the same
        // way. A PLACEMENT costs no IP weight and is counted per account, a
        // budget nothing in this process tracks; a CANCEL is metered per IP like
        // any other call and comes out of the pool the market-data half shares.
        const auto bucket =
            kind == Kind::Create ? exec::RateBucket::OrderEntry : exec::RateBucket::Shared;
        const auto cost = kind == Kind::Create ? k_cost_order : k_weight_cancel;
        if (const auto allowed = api_.spend(bucket, cost, exec::RateClass::Order);
            !allowed.has_value()) {
            emit_reject(
                cmd.client_order_id, cmd.instrument, cmd.side, cmd.qty, kind, allowed.error());
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
        // ON THE WIRE AND UNANSWERED. Recorded here rather than in the callback
        // because the callback is exactly what may never run.
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
        if (const auto allowed =
                api_.spend(exec::RateBucket::Shared, k_weight_cancel_batch, exec::RateClass::Order);
            !allowed.has_value()) {
            for (const auto& cmd : cmds) {
                emit_reject(cmd.client_order_id,
                            cmd.instrument,
                            cmd.side,
                            cmd.qty,
                            Kind::Cancel,
                            allowed.error());
            }
            return;
        }
        // COPIED, because the caller's span is a view over a buffer the gateway
        // reuses on its next turn and this answer arrives on a later one.
        std::vector<OrderCmd> batch{cmds.begin(), cmds.end()};
        if (!api_.send(*request, [this, batch](net::RestResponse&& response) {
                on_batch_response(std::move(response), batch);
            })) {
            for (const auto& cmd : cmds) {
                emit_reject(
                    cmd.client_order_id,
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

    // The whole-request verdict, shared by the single and batch paths. Returns
    // the venue's refusal when the body is one, and nothing when the body is a
    // payload to read.
    //
    // A BENIGN CANCEL IS STILL RETURNED HERE. It arrives as a whole-request
    // refusal — {"code":-3200} and no acknowledgement at all — so swallowing it
    // would send the caller on to parse an ack that is not there. Whether it is
    // benign is the caller's question, because only the caller knows the verb.
    [[nodiscard]] std::optional<exec::VenueError>
    request_failure(const net::RestResponse& response) const noexcept {
        const auto error = parse_rest_error(response.body);
        if (!error.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable answer");
        }
        if (error->code != k_ok) {
            return exec::VenueError::make(classify(error->code), error->code, error->msg);
        }
        if (response.status < 200 || response.status >= 300) {
            // A path WEEX does not route answers a body with no `code` at all,
            // which reads to the error parser as a success payload.
            return exec::VenueError::make(
                exec::ErrorClass::Terminal, response.status, "venue refused without a code");
        }
        return std::nullopt;
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) noexcept {
        // Any answer at all settles it, including a refusal: what the table
        // tracks is silence, not failure.
        if (!response.transport_failed()) {
            (void)in_flight_.resolve(cmd.client_order_id);
        } else {
            // NOT A REJECTION. The request may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position. The order stays in
            // the table and the sweep reports it as a gap.
            state_->sink->on_order_error(
                cmd.client_order_id,
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
                // The cancel arrived late rather than failing; a CancelRejected
                // would leave the OMS holding an order the venue has closed.
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
                        exec::VenueError::make(
                            exec::ErrorClass::Terminal, 0, "unreadable order acknowledgement"));
            return;
        }
        publish(cmd, kind, *ack);
    }

    void on_batch_response(net::RestResponse&& response,
                           const std::vector<OrderCmd>& batch) noexcept {
        for (const auto& cmd : batch) {
            if (!response.transport_failed()) {
                (void)in_flight_.resolve(cmd.client_order_id);
            }
        }
        if (response.transport_failed()) {
            for (const auto& cmd : batch) {
                state_->sink->on_order_error(
                    cmd.client_order_id,
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
                emit_reject(
                    cmd.client_order_id, cmd.instrument, cmd.side, cmd.qty, Kind::Cancel, *failure);
            }
            return;
        }
        static thread_local OrderAckBuffer storage;
        const auto acks = parse_order_acks(response.body, storage);
        if (!acks.has_value()) {
            for (const auto& cmd : batch) {
                emit_reject(cmd.client_order_id,
                            cmd.instrument,
                            cmd.side,
                            cmd.qty,
                            Kind::Cancel,
                            exec::VenueError::make(
                                exec::ErrorClass::Terminal, 0, "unreadable batch acknowledgement"));
            }
            return;
        }
        // MATCHED BY THE ID WE SENT, not by position: WEEX states no ordering
        // for the array, and a cancel published against the wrong order closes
        // a position the desk meant to keep. A command the answer never names
        // is left in doubt rather than reported either way.
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
                // The cancel arrived late rather than failing; a CancelRejected
                // would leave the OMS holding an order the venue has closed.
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
        // ACCEPTED, AND ONLY ACCEPTED. WEEX's place answer says the order was
        // taken and nothing about what it then did — an IOC that filled on
        // arrival looks exactly like a limit resting in the book — so the
        // leaves stated here are the whole order, and the fill that may already
        // have happened reaches this process through the reconciliation pull.
        emit(cmd, OrderEventType::Accepted, ack.order_id, cmd.qty);
    }

    void emit(const OrderCmd& cmd,
              OrderEventType type,
              std::string_view order_id,
              QtyLots leaves) noexcept {
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

    // The reason is not optional. Every refusal here reaches the sink with the
    // verdict that caused it, because the terminal event itself has no room for
    // one and a rejection nobody can explain costs an hour at the exchange.
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
        // A create that never reached the book leaves nothing behind; a refused
        // cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    void reject(const OrderCmd& cmd, Kind kind, BuildError why) noexcept {
        // A request that could not be SIGNED is an authentication failure and
        // not a bad order: the gateway must halt on it rather than reject one
        // command and take the next.
        const auto cls = why == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                      : exec::ErrorClass::Terminal;
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

} // namespace raikiri::venues::weex::detail
