#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/orders.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::gate::detail {

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
class GateExec final : public exec::ExecutionClient {
public:
    // WARNING: Gate names both markets' BTC_USDT identically, and the mappers
    // below do NOT name a market — map_bbo, map_trade, map_order_event and
    // map_user_trade all take (wire, table, ts), and WireOrder carries no
    // market field. So a process with BOTH catalogues loaded would have every
    // one of them answer nullptr and drop every frame, silently. Run one
    // gateway per market; the request builders are already market-correct.
    GateExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, api_{*state_} {
        // Gate's two private streams are different hosts with different order
        // shapes, so exactly one of them opens — the one this process serves.
        if (market == MarketType::Perp) {
            perp_ = std::make_unique<PrivateStream>(*state_, MarketType::Perp);
        } else if (market == MarketType::Spot) {
            spot_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot);
        }
    }

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (perp_ && state_->futures_user_id == 0) {
            // Fetched before the socket opens, because on_open cannot ask for
            // it: a subscribe with no user id is answered with an empty stream
            // rather than an error, and that reads as a quiet account.
            if (const auto user = api_.futures_user_id(); user.has_value()) {
                state_->futures_user_id = *user;
            }
            if (state_->futures_user_id == 0) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::AuthFailure, 0, "gate: futures account id unavailable")};
            }
        }
        if (perp_) {
            perp_->start();
        }
        if (spot_) {
            spot_->start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (perp_) {
            perp_->stop();
        }
        if (spot_) {
            spot_->stop();
        }
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        const bool perp_up = perp_ == nullptr || perp_->connected();
        const bool spot_up = spot_ == nullptr || spot_->connected();
        return started_ && perp_up && spot_up && (perp_ != nullptr || spot_ != nullptr);
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

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        // Gate caps a batch at 20 and refuses the whole call past it, so the
        // span is chunked rather than truncated.
        for (std::size_t at = 0; at < cmds.size(); at += SignedRequestBuilder::k_max_batch_cancel) {
            const auto chunk = cmds.subspan(
                at, std::min(SignedRequestBuilder::k_max_batch_cancel, cmds.size() - at));
            const auto request =
                api_.builder().cancel_batch(chunk, state_->instruments, state_->venue_now());
            if (!request.has_value()) {
                const auto refusal = build_failure(request.error());
                for (const auto& cmd : chunk) {
                    emit_reject(cmd, Kind::Cancel, refusal);
                }
                continue;
            }
            // The per-order verdicts come back in the body; a transport
            // failure is the only thing this callback can honestly report.
            std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
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
                    const auto refusal = rest_failure(response.body);
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

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError>
    funding_charges(MarketType market, UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

private:
    // What a command is asking the venue to do. The three share one path
    // because they share one failure: an unanswered request.
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

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

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        auto& builder = api_.builder();
        const auto request =
            kind == Kind::Create   ? builder.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder.cancel_order(cmd, state_->instruments, now)
                                   : builder.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        const auto market = cmd.instrument.market();
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
        if (response.status >= 200 && response.status < 300) {
            // The success body IS the order. Remembering it here seeds the
            // leaves before the socket's `_new` push arrives, which is what
            // lets a fill that overtakes that push still state its leaves.
            if (const auto wire = parse_rest_order(response.body, market); wire.has_value()) {
                state_->remember(*wire);
            }
            return;
        }
        if (kind == Kind::Cancel) {
            if (const auto err = parse_rest_error(response.body);
                err.has_value() && is_benign_cancel_failure(err->label)) {
                return; // the order already finished; reconciliation settles it
            }
        }
        emit_reject(cmd, kind, rest_failure(response.body));
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
    std::unique_ptr<PrivateStream> perp_;
    std::unique_ptr<PrivateStream> spot_;
    RestApi api_;
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<GateExec>(std::move(state), market);
}

} // namespace raikiri::venues::gate::detail
