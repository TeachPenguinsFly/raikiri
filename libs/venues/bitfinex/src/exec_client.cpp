#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::bitfinex::detail {

namespace {

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Where the socket meets the REST call.
//
// An order goes OUT over REST and comes BACK on the authenticated socket, and
// nothing on either side can tell whether the other happened. This class is the
// only thing that knows both: it holds the table of orders handed over and not
// yet answered, ages them out, and synthesises the gap event that stops an
// unanswered order sitting open forever in the strategy's book.
//
// REST is the write transport because every write here answers with the
// notification that says whether it took — Bitfinex ANSWERS HTTP 200 TO A
// REFUSED ORDER, and that array's STATUS field is the only verdict there is.
// The websocket accepts the same writes at lower latency and is the obvious
// next step; it answers on the same `n` frames, so the reader below already
// handles what it would produce.
class BitfinexExec final : public exec::ExecutionClient {
public:
    BitfinexExec(std::shared_ptr<VenueState> state, MarketType market);

    ~BitfinexExec() override { state_->private_poll = nullptr; }

    // This object registers itself in state the DATA client also holds, so a
    // copy would leave two registrations and one of them dangling.
    BitfinexExec(const BitfinexExec&) = delete;
    BitfinexExec& operator=(const BitfinexExec&) = delete;
    BitfinexExec(BitfinexExec&&) = delete;
    BitfinexExec& operator=(BitfinexExec&&) = delete;

    std::expected<void, exec::VenueError> start() override;

    void stop() override;

    [[nodiscard]] bool connected() const noexcept override;

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

    void fire(const OrderCmd& cmd, Kind kind) noexcept;

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind);

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
    std::unique_ptr<PrivateStream> stream_;
    RestApi api_;
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
    bool spot_{false};
    bool perp_{false};
    bool started_{false};
};

BitfinexExec::BitfinexExec(std::shared_ptr<VenueState> state, MarketType market)
    : state_{std::move(state)}, api_{*state_}, spot_{market == MarketType::Spot},
      perp_{market == MarketType::Perp} {}

std::expected<void, exec::VenueError> BitfinexExec::start() {
    api_.start();
    // The socket only carries a lifecycle once it is authenticated, and it
    // cannot authenticate without credentials — so an unconfigured venue keeps
    // its REST pool and reports itself unconnected rather than opening a socket
    // that will be refused on every reconnect.
    if (!state_->api_key.empty() && !state_->api_secret.empty()) {
        stream_ = std::make_unique<PrivateStream>(*state_);
        stream_->start();
    }
    // ExecutionClient has no poll hook the data client reaches, and the private
    // socket's ping has to ride the same reactor the public one pumps.
    state_->private_poll = [this](UnixNanos now) {
        if (stream_ != nullptr) {
            stream_->heartbeat(now);
        }
    };
    started_ = true;
    return {};
}

void BitfinexExec::stop() {
    state_->private_poll = nullptr;
    if (stream_) {
        stream_->stop();
    }
    api_.stop();
    started_ = false;
}

bool BitfinexExec::connected() const noexcept {
    return started_ && stream_ != nullptr && stream_->connected() && (spot_ || perp_);
}

void BitfinexExec::fire(const OrderCmd& cmd, Kind kind) noexcept {
    const auto now = state_->clock->wall();
    const auto request =
        kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
        : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                               : api_.builder().amend_order(cmd, state_->instruments, now);
    if (!request.has_value()) {
        emit_reject(cmd, kind, build_failure(request.error()));
        return;
    }
    if (kind == Kind::Create) {
        // Recorded before the request goes out: an `on` push can beat the REST
        // answer, and without the cid mapping it would arrive unattributable.
        if (const auto cid = cid_from(cmd.exchange_client_order_id); cid.has_value()) {
            state_->remember_cid(*cid, cmd.exchange_client_order_id);
        }
    }
    if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
            on_response(std::move(response), cmd, kind);
        })) {
        // Never left this process, so nothing is in doubt.
        emit_reject(
            cmd, kind, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
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

void BitfinexExec::on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
    // Any answer settles it, including a refusal: the table tracks silence.
    if (!response.transport_failed()) {
        (void)unanswered_.resolve(cmd.client_order_id);
    }
    if (response.transport_failed()) {
        // NOT A REJECTION. The request may have reached the matcher before the
        // read failed, and calling that a refusal is how a process records a
        // rejection opposite a live position.
        state_->sink->on_order_error(
            cmd.client_order_id,
            exec::VenueError::make(exec::ErrorClass::Connectivity,
                                   0,
                                   response.error.empty()
                                       ? std::string_view{"no response from venue"}
                                       : std::string_view{response.error}));
        return;
    }
    // An `["error",code,text]` body is the other shape a refusal arrives in,
    // and it names itself where the notification below would not.
    if (const auto refusal = rest_error(response.body); refusal.has_value()) {
        emit_reject(cmd, kind, *refusal);
        return;
    }
    // BITFINEX ANSWERS HTTP 200 TO A REFUSED ORDER. The notification's STATUS
    // is the verdict, and an adapter that read the status code instead would
    // report every rejection as an acceptance and leave the order open forever.
    const auto notification = parse_notification(response.body);
    if (!notification.has_value()) {
        emit_reject(
            cmd,
            kind,
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(notification.error())));
        return;
    }
    if (!notification_ok(*notification)) {
        emit_reject(cmd,
                    kind,
                    exec::VenueError::make(
                        classify(notification->code), notification->code, notification->text));
        return;
    }
    if (kind == Kind::Create && notification->has_order) {
        // The ack carries the order itself. Seeding the memo here is what lets
        // a fill that overtakes the socket's `on` push still state its leaves.
        state_->remember(notification->order.id,
                         {.client_order_id = cmd.exchange_client_order_id, .leaves = cmd.qty});
    }
    // The socket's push carries the lifecycle.
}

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<BitfinexExec>(std::move(state), market);
}

} // namespace raikiri::venues::bitfinex::detail
