#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/orders.hpp>
#include <raikiri/venues/bitmex/rest.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::bitmex::detail {

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
class BitmexExec final : public exec::ExecutionClient {
public:
    explicit BitmexExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signer.has_credentials()) {
            // Every path below signs. Saying so once at start beats one
            // AuthFailure per order for the rest of the session.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "bitmex: credentials not loaded")};
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

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        // BitMEX keys a cancel on the order id alone, so unlike most venues here
        // a batch may straddle instruments and only the count needs chunking.
        for (std::size_t at = 0; at < cmds.size(); at += SignedRequestBuilder::k_max_batch_cancel) {
            const auto chunk = cmds.subspan(
                at, std::min(SignedRequestBuilder::k_max_batch_cancel, cmds.size() - at));
            send_batch(chunk);
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
        const auto now = state_->clock->wall();
        auto request =
            kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                                   : api_.builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
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

    void send_batch(std::span<const OrderCmd> chunk) noexcept {
        auto request =
            api_.builder().cancel_batch(chunk, state_->instruments, state_->clock->wall());
        if (!request.has_value()) {
            const auto refusal = build_failure(request.error());
            for (const auto& cmd : chunk) {
                emit_reject(cmd, Kind::Cancel, refusal);
            }
            return;
        }
        std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
        if (!api_.send(*request, [this, pending](net::RestResponse&& response) {
                on_batch_response(std::move(response), pending);
            })) {
            // Never left this process, so nothing is in doubt.
            for (const auto& cmd : chunk) {
                emit_reject(
                    cmd,
                    Kind::Cancel,
                    exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            }
            return;
        }
        // ON THE WIRE AND UNANSWERED, every member of it: one lost read leaves
        // the whole chunk's outcome unknown, not refused.
        for (const auto& cmd : chunk) {
            (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                                   .exchange_client_order_id = cmd.exchange_client_order_id,
                                   .instrument = cmd.instrument,
                                   .side = cmd.side,
                                   .qty = cmd.qty,
                                   .kind = cmd.kind,
                                   .sent = state_->clock->mono()});
        }
    }

    void on_batch_response(net::RestResponse&& response, const std::vector<OrderCmd>& pending) {
        // Any answer settles it, including a refusal: the table tracks silence.
        if (!response.transport_failed()) {
            for (const auto& cmd : pending) {
                (void)unanswered_.resolve(cmd.client_order_id);
            }
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The chunk may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position.
            const auto error = exec::VenueError::make(
                exec::ErrorClass::Connectivity,
                0,
                response.error.empty() ? std::string_view{"no response from venue"}
                                       : std::string_view{response.error});
            for (const auto& cmd : pending) {
                state_->sink->on_order_error(cmd.client_order_id, error);
            }
            return;
        }
        if (response.status != k_ok) {
            const auto refusal = rest_failure(response);
            for (const auto& cmd : pending) {
                emit_reject(cmd, Kind::Cancel, refusal);
            }
            return;
        }
        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
        auto& storage = *storage_owner;
        const auto rows = parse_orders(response.body, storage);
        if (!rows.has_value()) {
            const auto refusal =
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable order rows");
            for (const auto& cmd : pending) {
                emit_reject(cmd, Kind::Cancel, refusal);
            }
            return;
        }
        // The per-order verdict is IN THE ROW: BitMEX answers HTTP 200 and puts
        // `error` on the ones it could not cancel. Reading the status line alone
        // reports every one of them as cancelled.
        for (const auto& wire : *rows) {
            if (wire.error.empty()) {
                publish_order(*state_, wire, FrameAction::Insert, state_->clock->wall());
                continue;
            }
            for (const auto& cmd : pending) {
                if (cmd.target_exchange_order_id.view() == wire.order_id) {
                    emit_reject(cmd,
                                Kind::Cancel,
                                exec::VenueError::make(exec::ErrorClass::Terminal, 0, wire.error));
                }
            }
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        const auto received = state_->clock->wall();
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
        if (response.status != k_ok) {
            // A cancel for an order BitMEX has already finished with is not a
            // failure worth telling the OMS about; reconciliation settles it.
            if (kind == Kind::Cancel && response.status == k_not_found) {
                return;
            }
            emit_reject(cmd, kind, rest_failure(response));
            return;
        }
        if (kind == Kind::Cancel) {
            on_batch_response(std::move(response), std::vector<OrderCmd>{cmd});
            return;
        }
        const auto wire = parse_order(response.body);
        if (!wire.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable order row"));
            return;
        }
        if (kind == Kind::Amend) {
            // ordStatus stays "New" through an amend, so the order table can
            // never report one. This response is the only confirmation there is;
            // without it an amended order stays PendingUpdate forever.
            publish_order(*state_, *wire, FrameAction::Update, received);
            emit_updated(*wire, cmd, received);
            return;
        }
        publish_order(*state_, *wire, FrameAction::Insert, received);
    }

    void emit_updated(const WireOrder& wire, const OrderCmd& cmd, UnixNanos received) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::Updated;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.price = cmd.price;
        event.qty = cmd.qty;
        // PUT /order takes a new TOTAL, and BitMEX answers with the row's own
        // leavesQty; that is what is still working, not the size that was asked.
        event.leaves_qty = cmd.qty;
        if (const auto* memo = state_->recall(std::string{wire.order_id}); memo != nullptr) {
            event.leaves_qty = memo->leaves;
        }
        event.ts_event = received;
        event.ts_init = received;
        state_->sink->on_order_event(event);
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
    return std::make_unique<BitmexExec>(std::move(state));
}

} // namespace raikiri::venues::bitmex::detail
