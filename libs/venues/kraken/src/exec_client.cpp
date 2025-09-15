#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/orders.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::kraken::detail {

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
class KrakenExec final : public exec::ExecutionClient {
public:
    explicit KrakenExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        if (state_->testnet) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kraken futures has no demo environment")};
        }
        if (!state_->signer.has_credentials()) {
            // Every path below signs. Saying so once at start beats one
            // AuthFailure per order for the rest of the session.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "kraken: credentials not loaded")};
        }
        api_.start();
        stream_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        stream_.stop();
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return started_ && stream_.connected();
    }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

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

private:
    // What a command is asking the venue to do. The three share one path
    // because they share one failure: an unanswered request.
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    static StatusKind status_kind(Kind kind) noexcept {
        switch (kind) {
        case Kind::Cancel:
            return StatusKind::Cancel;
        case Kind::Amend:
            return StatusKind::Edit;
        case Kind::Create:
            break;
        }
        return StatusKind::Send;
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
            // UNDEF, not zero: zero leaves would read as "nothing is working".
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        auto& builder = api_.builder();
        auto request = kind == Kind::Create   ? builder.create_order(cmd, state_->instruments, now)
                       : kind == Kind::Cancel ? builder.cancel_order(cmd, state_->instruments, now)
                                              : builder.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            emit_reject(cmd,
                        kind,
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
        if (response.status < 200 || response.status >= 300 || !envelope.has_value() ||
            !envelope->ok) {
            if (kind == Kind::Cancel && envelope.has_value() &&
                is_benign_cancel_failure(envelope->error)) {
                return; // the order already finished; reconciliation settles it
            }
            emit_reject(cmd, kind, rest_failure(response.body));
            return;
        }
        // The envelope says "success" for a refused order as readily as for an
        // accepted one; `status` is the verdict.
        const auto verdict = parse_order_status(response.body, status_kind(kind));
        if (!verdict.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "no status in the response"));
            return;
        }
        switch (kind) {
        case Kind::Create:
            if (!is_send_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
                return;
            }
            // Accepted is published by the open_orders delta, which carries the
            // venue's own view of the resting order. Binding the id here is
            // what lets a fill that overtakes that delta still route.
            bind_ack(verdict->order_id, cmd);
            return;
        case Kind::Cancel:
            if (!is_cancel_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
            }
            return;
        case Kind::Amend:
            if (!is_edit_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
                return;
            }
            // The open_orders feed has no documented `reason` for an edit, so
            // this response is the only confirmation there is; without it an
            // amended order stays PendingUpdate forever.
            emit_updated(verdict->order_id, cmd);
            return;
        }
    }

    void bind_ack(std::string_view order_id, const OrderCmd& cmd) {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            return;
        }
        state_->remember(order_id,
                         LiveOrder{.instrument = cmd.instrument,
                                   .client_order_id = cmd.exchange_client_order_id,
                                   .side = cmd.side,
                                   .order_type = cmd.order_type,
                                   .flags = cmd.flags,
                                   .price = cmd.price,
                                   .qty = cmd.qty,
                                   .leaves = cmd.qty});
    }

    void emit_updated(std::string_view order_id, const OrderCmd& cmd) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::Updated;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        if (const auto id = ExchangeOrderId::from(order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.price = cmd.price;
        event.qty = cmd.qty;
        // editorder RESIZES rather than tops up: `size` is the new total, so
        // what is left of it is the whole of it minus what already traded, and
        // the fills feed is what walks it down from here.
        event.leaves_qty = cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
        if (const auto* memo = state_->recall(order_id); memo != nullptr) {
            auto updated = *memo;
            updated.price = cmd.price;
            updated.qty = cmd.qty;
            updated.leaves = cmd.qty;
            state_->remember(order_id, updated);
        }
    }

    // The reason is not optional. `OrderEvent` has no room for text, so every
    // refusal hands the sink the verdict that caused it — otherwise an order
    // that dies here is indistinguishable from one that was never sent.
    void emit_reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        // A create that never reached the book leaves nothing behind; a refused
        // cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    PrivateStream stream_;
    RestApi api_;
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<KrakenExec>(std::move(state));
}

} // namespace raikiri::venues::kraken::detail
