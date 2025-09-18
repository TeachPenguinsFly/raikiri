#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/venues/xt/rest.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::xt::detail {

namespace {

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Where the socket meets the REST call.
//
// Order entry goes OUT over REST for both markets and the lifecycle comes BACK
// on the matching private socket, and nothing on either side can tell whether
// the other happened. This class is the only thing that knows both: it holds
// the table of orders handed over and not yet answered, ages them out, and
// synthesises the gap event that stops an unanswered order sitting open
// forever in the strategy's book. The REST answer to a create carries only the
// venue order id — the socket's `NEW` push is the acceptance — so nothing is
// emitted on a success here beyond seeding the memo that later fills read
// their side and leaves out of.
class XtExec final : public exec::ExecutionClient {
public:
    XtExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, api_{*state_, market}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {}

    ~XtExec() override { state_->private_poll = nullptr; }

    // This object registers itself in state the DATA client also holds, so a
    // copy would leave two registrations and one of them dangling.
    XtExec(const XtExec&) = delete;
    XtExec& operator=(const XtExec&) = delete;
    XtExec(XtExec&&) = delete;
    XtExec& operator=(XtExec&&) = delete;

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (perp_) {
            perp_stream_ = std::make_unique<PrivateStream>(*state_, MarketType::Perp);
            if (mint_listen_key()) {
                perp_stream_->start();
            }
        }
        if (spot_) {
            spot_stream_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot);
            if (mint_ws_token()) {
                spot_stream_->start();
            }
        }
        state_->private_poll = [this](UnixNanos now) { poll_private(now); };
        started_ = true;
        return {};
    }

    void stop() override {
        state_->private_poll = nullptr;
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
        return started_ && perp_up && spot_up && (spot_ || perp_);
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
        if (http_ok && envelope_ok(response.body, market)) {
            if (kind == Kind::Create) {
                // The ack carries the venue order id and nothing else. Seeding
                // the memo here is what lets a fill that overtakes the socket's
                // NEW push still state its side and its leaves.
                if (const auto id = parse_order_ack(response.body); id.has_value()) {
                    state_->remember(*id,
                                     {.client_order_id = cmd.exchange_client_order_id,
                                      .leaves = cmd.qty,
                                      .side = cmd.side,
                                      .side_known = true});
                }
            }
            return; // the socket's push carries the lifecycle
        }
        if (kind == Kind::Cancel) {
            const auto code =
                market == MarketType::Spot
                    ? parse_spot_envelope(response.body).transform([](const SpotEnvelope& e) {
                          return e.mc;
                      })
                    : parse_rest_envelope(response.body).transform([](const RestEnvelope& e) {
                          return e.code;
                      });
            if (code.has_value() && is_benign_cancel_failure(*code)) {
                return; // the order already finished; reconciliation settles it
            }
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
        // A create that never reached the book leaves nothing behind; a refused
        // cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    // Minted synchronously, and only at start(): the socket cannot subscribe
    // without a key, and a cold start is the one place a blocking round trip
    // costs nothing. A FAILURE MARKS THE KEY STALE so the poll below re-mints
    // it without ever blocking the gateway loop.
    [[nodiscard]] bool mint_listen_key() {
        const auto request = api_.builder().listen_key(state_->venue_now(MarketType::Perp));
        const auto key =
            request.has_value()
                ? api_.fetch(*request, MarketType::Perp).and_then([](const std::string& body) {
                      const auto parsed = parse_listen_key(body);
                      return parsed.has_value()
                                 ? std::expected<std::string, exec::VenueError>{std::string{
                                       *parsed}}
                                 : std::unexpected{exec::VenueError::make(
                                       exec::ErrorClass::Terminal, 0, "no key")};
                  })
                : std::unexpected{build_failure(request.error())};
        state_->perp_key_stale = !key.has_value();
        if (!key.has_value()) {
            return false;
        }
        perp_stream_->set_key(*key);
        return true;
    }

    [[nodiscard]] bool mint_ws_token() {
        const auto request = api_.builder().ws_token(state_->venue_now(MarketType::Spot));
        const auto token =
            request.has_value()
                ? api_.fetch(*request, MarketType::Spot).and_then([](const std::string& body) {
                      const auto parsed = parse_spot_ws_token(body);
                      return parsed.has_value()
                                 ? std::expected<std::string, exec::VenueError>{std::string{
                                       *parsed}}
                                 : std::unexpected{exec::VenueError::make(
                                       exec::ErrorClass::Terminal, 0, "no token")};
                  })
                : std::unexpected{build_failure(request.error())};
        state_->spot_key_stale = !token.has_value();
        if (!token.has_value()) {
            return false;
        }
        spot_stream_->set_key(*token);
        return true;
    }

    // A key XT declared invalid is re-minted ASYNCHRONOUSLY: this runs on the
    // gateway's poll, and the blocking fetch() that start() can afford would
    // stall every other venue for the length of one XT round trip.
    void refresh_key(MarketType market) noexcept {
        auto* stream = market == MarketType::Spot ? spot_stream_.get() : perp_stream_.get();
        bool& in_flight = market == MarketType::Spot ? spot_key_in_flight_ : perp_key_in_flight_;
        if (stream == nullptr || in_flight) {
            return;
        }
        const auto now = state_->venue_now(market);
        const auto request = market == MarketType::Spot ? api_.builder().ws_token(now)
                                                        : api_.builder().listen_key(now);
        if (!request.has_value()) {
            return; // no credentials: nothing to retry with
        }
        in_flight = true;
        if (!api_.send(*request, market, [this, market](net::RestResponse&& response) {
                on_key(std::move(response), market);
            })) {
            in_flight = false;
        }
    }

    void on_key(net::RestResponse&& response, MarketType market) {
        auto* stream = market == MarketType::Spot ? spot_stream_.get() : perp_stream_.get();
        (market == MarketType::Spot ? spot_key_in_flight_ : perp_key_in_flight_) = false;
        if (stream == nullptr || response.transport_failed() ||
            !envelope_ok(response.body, market)) {
            return; // still stale; the next poll tries again
        }
        const auto key = market == MarketType::Spot ? parse_spot_ws_token(response.body)
                                                    : parse_listen_key(response.body);
        if (!key.has_value()) {
            return;
        }
        stream->set_key(*key);
        (market == MarketType::Spot ? state_->spot_key_stale : state_->perp_key_stale) = false;
        stream->reconnect();
    }

    // Driven by the data client's poll: ExecutionClient has no hook of its own
    // and XT's 30 s idle timer counts only what the client sent.
    void poll_private(UnixNanos now) noexcept {
        if (state_->perp_key_stale) {
            refresh_key(MarketType::Perp);
        }
        if (state_->spot_key_stale) {
            refresh_key(MarketType::Spot);
        }
        if (perp_stream_) {
            perp_stream_->keepalive(now);
        }
        if (spot_stream_) {
            spot_stream_->keepalive(now);
        }
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> perp_stream_;
    std::unique_ptr<PrivateStream> spot_stream_;
    RestApi api_;
    bool perp_key_in_flight_{false};
    bool spot_key_in_flight_{false};
    bool spot_{false};
    bool perp_{false};
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<XtExec>(std::move(state), market);
}

} // namespace raikiri::venues::xt::detail
