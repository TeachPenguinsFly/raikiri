#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/orders.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::mexc::internal {

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
//
// Order entry over REST for both markets. MEXC has historically gated futures
// order placement behind an account permission: the legacy contract API closed
// its place/cancel endpoints in July 2022 and MEXC reopened API futures trading
// on 2026-03-31 behind KYC plus an explicit "Order Placing" grant on the key. A
// key without that grant answers a permission code (701-704), which classifies
// as AuthFailure — the order is refused loudly rather than left open.
class MexcExec final : public exec::ExecutionClient {
public:
    MexcExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, api_{*state_, market}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {
        if (perp_) {
            stream_ = std::make_unique<PrivateStream>(*state_);
            state_->private_client = &stream_->client();
        }
    }

    ~MexcExec() override {
        if (state_->private_client == (stream_ ? &stream_->client() : nullptr)) {
            state_->private_client = nullptr;
        }
    }

    // This object registers itself in state the DATA client also holds, so a
    // copy would leave two registrations and one of them dangling.
    MexcExec(const MexcExec&) = delete;
    MexcExec& operator=(const MexcExec&) = delete;
    MexcExec(MexcExec&&) = delete;
    MexcExec& operator=(MexcExec&&) = delete;

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (stream_) {
            stream_->start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (stream_) {
            stream_->stop();
        }
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        // Spot has no JSON user stream to be up or down, so an account trading
        // spot alone is connected once its REST pool is.
        const bool stream_up = stream_ == nullptr || stream_->connected();
        return started_ && stream_up && (spot_ || perp_);
    }

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

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        if (cmds.empty()) {
            return;
        }
        // Spot has no cancel-by-id-list at all, and futures caps its array, so
        // neither market may be truncated into one call.
        if (cmds.front().instrument.market() == MarketType::Spot) {
            for (const auto& cmd : cmds) {
                cancel(cmd);
            }
            return;
        }
        for (std::size_t at = 0; at < cmds.size(); at += k_max_batch_cancel) {
            const auto chunk = cmds.subspan(at, std::min(k_max_batch_cancel, cmds.size() - at));
            const auto request = api_.builder().cancel_batch(
                chunk, state_->instruments, state_->venue_now(MarketType::Perp));
            if (!request.has_value()) {
                // An order known only by our own id cannot ride the array
                // endpoint; one call each still cancels it.
                for (const auto& cmd : chunk) {
                    cancel(cmd);
                }
                continue;
            }
            // The per-order verdicts come back in the body; a transport
            // failure is the only thing this callback can honestly report.
            const std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
            if (!api_.send(*request, [this, pending](net::RestResponse&& response) {
                    if (!response.transport_failed() && response.status >= 200 &&
                        response.status < 300) {
                        return;
                    }
                    // A LOST CONNECTION IS WORSE IN A BATCH THAN ALONE: it would declare
                    // every cancel in the chunk refused while the venue may have taken all
                    // of them, leaving the desk sure it still holds orders it does not.
                    if (response.transport_failed()) {
                        for (const auto& cmd : pending) {
                            state_->sink->on_order_error(
                                cmd.client_order_id,
                                exec::VenueError::make(
                                    exec::ErrorClass::Connectivity,
                                    0,
                                    response.error.empty()
                                        ? std::string_view{"no response from venue"}
                                        : std::string_view{response.error}));
                        }
                        return;
                    }
                    const auto refusal =
                        rest_failure(response.body, pending.front().instrument.market());
                    for (const auto& cmd : pending) {
                        (void)unanswered_.resolve(cmd.client_order_id);
                        emit_reject(cmd, Kind::Cancel, refusal);
                    }
                })) {
                for (const auto& cmd : chunk) {
                    emit_reject(
                        cmd,
                        Kind::Cancel,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
                }
            }
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
    // What a command is asking the venue to do. The three share one path
    // because they share one failure: an unanswered request.
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto market = cmd.instrument.market();
        const auto now = state_->venue_now(market);
        const auto request =
            kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                                   : api_.builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!api_.send(*request, [this, cmd, kind, market](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind, market);
            })) {
            // Never left this process, so nothing is in doubt.
            emit_reject(cmd,
                        kind,
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

    void
    on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind, MarketType market) {
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
        const bool http_ok = response.status >= 200 && response.status < 300;
        if (market == MarketType::Spot) {
            if (!http_ok) {
                if (kind == Kind::Cancel) {
                    if (const auto err = parse_spot_error(response.body);
                        err.has_value() && is_benign_spot_cancel_failure(err->code)) {
                        return; // the order already finished; reconciliation settles it
                    }
                }
                emit_reject(cmd, kind, rest_failure(response.body, market));
                return;
            }
            // Spot has no JSON user stream, so the create response IS the
            // acknowledgement: without it the order never leaves Submitted.
            if (kind == Kind::Create) {
                if (const auto wire = parse_spot_order(response.body); wire.has_value()) {
                    if (const auto event =
                            map_spot_order_event(*wire, state_->instruments, state_->clock->wall());
                        event.has_value()) {
                        state_->sink->on_order_event(*event);
                    }
                }
            }
            return;
        }

        // Futures answers 200 with success:false, so an HTTP 2xx is not a fill
        // of the request — the envelope's code is.
        const auto envelope = parse_rest_envelope(response.body);
        const bool accepted =
            http_ok && envelope.has_value() && envelope->success && envelope->code == k_ok;
        if (accepted) {
            return; // the socket's push carries the lifecycle
        }
        if (kind == Kind::Cancel && envelope.has_value() &&
            is_benign_cancel_failure(envelope->code)) {
            return;
        }
        emit_reject(cmd, kind, rest_failure(response.body, market));
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
        // A create that never reached the book leaves nothing behind; a
        // refused cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> stream_;
    RestApi api_;
    bool spot_{false};
    bool perp_{false};
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<MexcExec>(std::move(state), market);
}

} // namespace raikiri::venues::mexc::internal
