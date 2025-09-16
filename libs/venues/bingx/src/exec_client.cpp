#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/orders.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::bingx::detail {

namespace {

// A listen key is valid one hour and BingX recommends extending every thirty
// minutes. Twenty leaves room for a missed tick and a failed extension inside
// the window rather than discovering the key is dead from a listenKeyExpired
// push with orders working.
constexpr std::uint64_t k_listen_key_interval_ns = 20ULL * 60 * 1'000'000'000ULL;

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Where the socket meets the REST call.
//
// An order goes OUT over REST and comes BACK on the user stream, and nothing
// on either side can tell whether the other happened. This class is the only
// thing that knows both: it holds the table of orders handed over and not yet
// answered, ages them out, and synthesises the gap event that stops an
// unanswered order sitting open forever in the strategy's book.
//
// It also owns the listen key, because the key is what the private socket
// authenticates with and REST is the only place to mint one: neither half can
// hold it alone.
//
// Order entry over REST on both trees, with the lifecycle arriving on the
// matching user stream. A successful create emits nothing: it only seeds the
// memo a later fill reads its own id out of.
class BingxExec final : public exec::ExecutionClient {
public:
    BingxExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, api_{*state_}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {}

    ~BingxExec() override { state_->private_poll = nullptr; }

    // This object registers itself in state the DATA client also holds, so a
    // copy would leave two registrations and one of them dangling.
    BingxExec(const BingxExec&) = delete;
    BingxExec& operator=(const BingxExec&) = delete;
    BingxExec(BingxExec&&) = delete;
    BingxExec& operator=(BingxExec&&) = delete;

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (perp_) {
            api_.read_position_mode();
        }
        if (mint_listen_key()) {
            open_streams();
        }
        state_->private_poll = [this](UnixNanos) { poll_private(); };
        started_ = true;
        return {};
    }

    void stop() override {
        state_->private_poll = nullptr;
        // Best effort, and before the pool closes: a key left open keeps a
        // socket slot alive on BingX's side for an hour after we are gone.
        if (started_ && !key_.empty()) {
            if (const auto request = api_.builder().listen_key_close(key_, state_->venue_now());
                request.has_value()) {
                (void)api_.send(*request, [](net::RestResponse&&) {});
                state_->io.poll();
            }
        }
        perp_stream_.reset();
        spot_stream_.reset();
        key_.clear();
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        if (!started_ || !(spot_ || perp_)) {
            return false;
        }
        if (perp_ && (perp_stream_ == nullptr || !perp_stream_->connected())) {
            return false;
        }
        if (spot_ && (spot_stream_ == nullptr || !spot_stream_->connected())) {
            return false;
        }
        return true;
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
            // UNDEF, not zero: zero leaves would read as "nothing is working".
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
        // Deliberately one call each. BingX's array endpoints exist, but its
        // own documentation spells their client-id list two ways on one page,
        // and a batch naming the field BingX does not bind cancels NOTHING
        // while answering success.
        for (const auto& cmd : cmds) {
            cancel(cmd);
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

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                                   : api_.builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
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
        const bool http_ok = response.status >= 200 && response.status < 300;
        const bool accepted = http_ok && envelope.has_value() && envelope->code == k_ok;
        if (accepted) {
            if (kind == Kind::Create) {
                // The ack carries the venue order id and little else. Seeding
                // the memo here is what lets a spot fill — whose push names no
                // client order id at all — still say whose it is.
                if (const auto ack = parse_order_ack(response.body); ack.has_value()) {
                    state_->remember(ack->order_id, cmd.exchange_client_order_id);
                }
            }
            return; // the user stream carries the lifecycle
        }
        if (kind == Kind::Cancel && envelope.has_value() &&
            is_benign_cancel_failure(envelope->code)) {
            return; // the order already finished; reconciliation settles it
        }
        if (!envelope.has_value()) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(
                            exec::ErrorClass::Terminal, response.status, "unparseable envelope"));
            return;
        }
        emit_reject(
            cmd,
            kind,
            exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg));
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

    // Blocking, and only at start(): the sockets cannot connect without a key
    // and a cold start is the one place a round trip costs nothing. ONE key
    // serves both trees — the endpoint is account-level, not per-market.
    [[nodiscard]] bool mint_listen_key() {
        const auto request = api_.builder().listen_key_create(state_->venue_now());
        if (!request.has_value()) {
            state_->key_stale = true;
            return false;
        }
        const auto response = api_.fetch_raw(*request);
        // This endpoint answers in HTTP statuses and no envelope at all, so
        // fetch() cannot read it: 404 here is a key BingX does not know, not a
        // path that moved (VERIFIED against production with a made-up key).
        if (!response.has_value() || response->transport_failed() || response->status != 200) {
            state_->key_stale = true;
            return false;
        }
        const auto key = parse_listen_key(response->body);
        if (!key.has_value() || key->empty()) {
            state_->key_stale = true;
            return false;
        }
        key_.assign(*key);
        state_->key_stale = false;
        key_due_ns_ = state_->clock->mono().ns + k_listen_key_interval_ns;
        return true;
    }

    void open_streams() {
        if (key_.empty()) {
            return; // a socket cannot authenticate without one
        }
        if (perp_) {
            perp_stream_ = std::make_unique<PrivateStream>(*state_, MarketType::Perp, key_);
            perp_stream_->start();
        }
        if (spot_) {
            spot_stream_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot, key_);
            spot_stream_->start();
        }
    }

    // Asynchronous on purpose: this runs on the gateway's poll, where one
    // BingX timeout would stall every other venue for the length of it.
    void refresh_key() noexcept {
        if (key_in_flight_) {
            return;
        }
        const auto request = api_.builder().listen_key_create(state_->venue_now());
        if (!request.has_value()) {
            return; // no credentials: nothing to retry with
        }
        key_in_flight_ = true;
        if (!api_.send(*request,
                       [this](net::RestResponse&& response) { on_key(std::move(response)); })) {
            key_in_flight_ = false;
        }
    }

    void on_key(net::RestResponse&& response) {
        key_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return; // still stale; the next poll tries again
        }
        const auto key = parse_listen_key(response.body);
        if (!key.has_value() || key->empty()) {
            return;
        }
        key_.assign(*key);
        state_->key_stale = false;
        key_due_ns_ = state_->clock->mono().ns + k_listen_key_interval_ns;
        // The key rides the URL, so a fresh key only takes effect on a fresh
        // connection. The rebuild waits for the next tick rather than
        // destroying two sockets from inside an io completion handler.
        rebuild_streams_ = true;
    }

    void poll_private() noexcept {
        if (!started_) {
            return;
        }
        if (rebuild_streams_) {
            rebuild_streams_ = false;
            perp_stream_.reset();
            spot_stream_.reset();
            open_streams();
            return;
        }
        if (state_->key_stale) {
            refresh_key();
            return;
        }
        if (key_.empty() || state_->clock->mono().ns < key_due_ns_ || keepalive_in_flight_) {
            return;
        }
        key_due_ns_ = state_->clock->mono().ns + k_listen_key_interval_ns;
        const auto request = api_.builder().listen_key_keepalive(key_, state_->venue_now());
        if (!request.has_value()) {
            return;
        }
        keepalive_in_flight_ = true;
        if (!api_.send(*request, [this](net::RestResponse&& response) {
                keepalive_in_flight_ = false;
                // 404 is BingX saying the key is gone. Anything else, including
                // a transport failure, is a tick to retry rather than a reason
                // to tear two sockets down.
                if (!response.transport_failed() && response.status == 404) {
                    state_->key_stale = true;
                }
            })) {
            keepalive_in_flight_ = false;
        }
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> perp_stream_;
    std::unique_ptr<PrivateStream> spot_stream_;
    RestApi api_;
    std::string key_;
    std::uint64_t key_due_ns_{0};
    bool key_in_flight_{false};
    bool rebuild_streams_{false};
    bool keepalive_in_flight_{false};
    bool spot_{false};
    bool perp_{false};
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<BingxExec>(std::move(state), market);
}

} // namespace raikiri::venues::bingx::detail
