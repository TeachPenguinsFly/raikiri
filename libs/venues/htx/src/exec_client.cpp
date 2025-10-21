#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/orders.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::htx::detail {

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
class HtxExec final : public exec::ExecutionClient {
public:
    HtxExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, api_{*state_, market} {
        if (market == MarketType::Perp) {
            perp_stream_ = std::make_unique<PrivateStream<SwapPrivateHandler>>(
                *state_, stream_host(), notify_path());
        }
        if (market == MarketType::Spot) {
            spot_stream_ = std::make_unique<PrivateStream<SpotPrivateHandler>>(
                *state_, spot_stream_host(), spot_private_path());
        }
    }

    std::expected<void, exec::VenueError> start() override {
        if (const auto ready = api_.start(); !ready.has_value()) {
            return std::unexpected{ready.error()};
        }
        if (perp_stream_) {
            perp_stream_->start();
        }
        if (spot_stream_) {
            spot_stream_->start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (perp_stream_) {
            perp_stream_->stop();
        }
        if (spot_stream_) {
            spot_stream_->stop();
        }
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        const bool perp_up = perp_stream_ == nullptr || perp_stream_->connected();
        const bool spot_up = spot_stream_ == nullptr || spot_stream_->connected();
        return started_ && perp_up && spot_up &&
               (perp_stream_ != nullptr || spot_stream_ != nullptr);
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
        // HTX takes one contract_code beside the id list, so a batch is
        // grouped by instrument before it is chunked by the venue's cap.
        std::vector<OrderCmd> group;
        for (std::size_t at = 0; at < cmds.size();) {
            group.clear();
            const auto instrument = cmds[at].instrument;
            const auto cap = instrument.market() == MarketType::Spot
                                 ? SignedRequestBuilder::k_max_spot_batch_cancel
                                 : SignedRequestBuilder::k_max_swap_batch_cancel;
            for (std::size_t i = at; i < cmds.size() && group.size() < cap; ++i) {
                if (cmds[i].instrument == instrument) {
                    group.push_back(cmds[i]);
                }
            }
            send_batch(group);
            // Advance past everything just taken, leaving other instruments in
            // place for the next pass.
            std::size_t consumed = 0;
            for (std::size_t i = at; i < cmds.size() && consumed < group.size(); ++i) {
                if (cmds[i].instrument == instrument) {
                    ++consumed;
                    at = i + 1;
                }
            }
            if (group.empty()) {
                ++at; // nothing matched: an impossible state, but not a spin
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

    void send_batch(std::span<const OrderCmd> group) noexcept {
        if (group.empty()) {
            return;
        }
        const auto market = group.front().instrument.market();
        const auto request =
            api_.builder().cancel_batch(group, state_->instruments, state_->venue_now());
        if (!request.has_value()) {
            const auto refusal = build_failure(request.error());
            for (const auto& cmd : group) {
                emit_reject(cmd, Kind::Cancel, refusal);
            }
            return;
        }
        // The per-order verdicts come back in the body's `errors` array; a
        // transport failure is the only thing this callback can honestly
        // report about all of them at once.
        std::vector<OrderCmd> pending{group.begin(), group.end()};
        if (!api_.send(*request, market, [this, pending](net::RestResponse&& response) {
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
                            exec::VenueError::make(exec::ErrorClass::Connectivity,
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
            for (const auto& cmd : group) {
                emit_reject(
                    cmd,
                    Kind::Cancel,
                    exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            }
        }
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                                   : api_.builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        const auto market = cmd.instrument.market();
        if (kind == Kind::Create && market != MarketType::Spot) {
            remember_swap_target(cmd);
        }
        if (!api_.send(*request, market, [this, cmd, kind, market](net::RestResponse&& response) {
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

    // The ULID never reaches the swap, so this is the only record tying the id
    // the OMS asks about to the int64 the venue was given.
    void remember_swap_target(const OrderCmd& cmd) {
        const auto numeric = swap_client_id(cmd.client_order_id);
        const auto symbol = state_->instruments.exchange_symbol(cmd.instrument);
        if (!numeric.has_value() || symbol.empty() || cmd.exchange_client_order_id.empty()) {
            return;
        }
        if (state_->swap_targets.size() >= k_max_live_orders) {
            return;
        }
        state_->swap_targets[std::string{cmd.exchange_client_order_id.view()}] = {
            .client_order_id = *numeric, .contract_code = std::string{symbol}};
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
        const auto envelope = parse_rest_envelope(response.body);
        const bool ok =
            response.status >= 200 && response.status < 300 && envelope.has_value() && envelope->ok;
        if (ok) {
            if (kind == Kind::Create) {
                // Binding the venue id here, off the create response, is what
                // lets a fill that overtakes the socket's push still route.
                bind_ack(response.body, cmd, market);
                return;
            }
            if (kind != Kind::Cancel) {
                return;
            }
            // A cancel that reached the venue still has to be read: HTX answers
            // status "ok" whether or not the order was cancelled, and puts the
            // per-id verdict in the body. Trusting the envelope leaves a live
            // order the OMS believes is gone.
            const auto outcome = parse_cancel_outcome(response.body, market);
            if (!outcome.has_value() || outcome->accepted ||
                is_benign_cancel_failure(outcome->code)) {
                return;
            }
            emit_reject(
                cmd, kind, exec::VenueError::make(exec::ErrorClass::Terminal, 0, outcome->code));
            return;
        }
        if (kind == Kind::Cancel && envelope.has_value() &&
            is_benign_cancel_failure(envelope->code)) {
            return; // the order already finished; reconciliation settles it
        }
        emit_reject(cmd, kind, rest_failure(response.body));
    }

    void bind_ack(std::string_view body, const OrderCmd& cmd, MarketType market) {
        const auto ack = parse_order_ack(body, market);
        if (!ack.has_value() || ack->order_id.empty()) {
            return;
        }
        if (state_->live_orders.size() >= k_max_live_orders) {
            return;
        }
        auto& memo = state_->live_orders[std::string{ack->order_id}];
        memo.leaves = cmd.qty;
        memo.client_order_id = cmd.client_order_id;
        if (market == MarketType::Spot) {
            memo.exchange_client_order_id = cmd.exchange_client_order_id;
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
        // A create that never reached the book leaves nothing behind; a
        // refused cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream<SwapPrivateHandler>> perp_stream_;
    std::unique_ptr<PrivateStream<SpotPrivateHandler>> spot_stream_;
    RestApi api_;
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<HtxExec>(std::move(state), market);
}

} // namespace raikiri::venues::htx::detail
