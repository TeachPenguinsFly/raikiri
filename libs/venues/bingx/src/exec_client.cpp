#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bingx::detail {

namespace {

// A listen key is valid one hour and BingX recommends extending every thirty
// minutes. Twenty leaves room for a missed tick and a failed extension inside
// the window rather than discovering the key is dead from a listenKeyExpired
// push with orders working.
constexpr std::uint64_t k_listen_key_interval_ns = 20ULL * 60 * 1'000'000'000ULL;

constexpr std::uint32_t k_perp_stream_id = 1;

constexpr std::uint32_t k_spot_stream_id = 2;

// The spot user stream is the one that needs asking; the perpetual one pushes
// everything as soon as the listenKey connects.
constexpr std::string_view k_spot_user_channel = "spot.executionReport";

// One account event. `market` is the socket's, not the frame's: BingX spells a
// spot pair and a perpetual identically and neither push says which tree it
// came from, so the connection is the only thing that knows.
void handle_private_frame(VenueState& state,
                          MarketType market,
                          std::string_view frame,
                          UnixNanos received) {
    const auto kind = parse_private_event(frame);
    if (!kind.has_value()) {
        return;
    }
    const auto stream_id = market == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
    switch (*kind) {
    case PrivateKind::OrderUpdate: {
        const auto wire = parse_order_update(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto fallback = state.remembered(wire->order_id);
        const auto event = map_order_update(*wire, state.instruments, market, fallback, received);
        if (!event.has_value()) {
            return;
        }
        if (!event->exchange_client_order_id.empty()) {
            state.remember(wire->order_id, event->exchange_client_order_id);
        }
        state.sink->on_order_event(*event);
        // Nothing more will arrive for an order the venue has closed, so the
        // memo entry goes with it rather than outliving the order forever.
        if (event->type == OrderEventType::Canceled || event->type == OrderEventType::Expired ||
            (event->type == OrderEventType::Filled && event->leaves_qty.raw == 0)) {
            state.forget(wire->order_id);
        }
        return;
    }
    case PrivateKind::ListenKeyExpired:
        state.key_stale = true;
        state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateKind::Error:
        if (const auto control = parse_control(frame); control.has_value()) {
            state.last_error_code = control->code;
        }
        state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateKind::Ack:
        state.sink->on_connectivity({.state = exec::ConnState::Connected,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateKind::Ignorable:
    case PrivateKind::Unknown:
        return;
    }
}

// The user stream. Same gzip framing and same "Ping" keepalive as the public
// socket — the authentication is the listenKey on the URL, which is why this
// handler carries no login frame.
class PrivateHandler final : public net::WsHandler {
public:
    PrivateHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        const auto stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
        if (market_ == MarketType::Spot && client_ != nullptr) {
            // Perpetual pushes everything unbidden; a silent SPOT socket is a
            // missing subscribe and not a quiet account.
            const auto frame = protocol_.subscribe_channel(k_spot_user_channel);
            if (!frame.has_value() || !client_->send(frame->text)) {
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = stream_id,
                                               .ts = state_->clock->wall()});
                return;
            }
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = inflater_.inflate(payload);
            if (!frame.has_value()) {
                ++state_->inflate_failures;
                return;
            }
            // Same keepalive as the public socket: four bytes, not JSON.
            if (is_ping(*frame)) {
                if (client_ != nullptr) {
                    (void)client_->send(Protocol::pong_frame());
                }
                return;
            }
            handle_private_frame(*state_, market_, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected,
             .stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id,
             .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    MarketType market_;
    Protocol protocol_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

// One private connection. The key is baked into the target, so a fresh key
// means a fresh stream object rather than a fresh subscribe frame.
struct PrivateStream {
    PrivateStream(VenueState& state, MarketType market, const std::string& key)
        : handler{state, market},
          client{state.io,
                 net::WsConfig{.host = std::string{private_stream_host(market)},
                               .port = "443",
                               .target = private_stream_target(market, key)},
                 handler} {
        handler.attach(client);
    }

    PrivateHandler handler;
    net::WsClient client;
};

// A build that failed for want of credentials is an AuthFailure, not a
// Terminal one: the gateway must halt and say so rather than retry a request
// that will fail identically forever.
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Answered BEFORE the instrument table is read.
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    const auto refusal = market_refusal(market);
    if (!refusal.has_value()) {
        return std::nullopt;
    }
    return exec::not_implemented(to_string(*refusal));
}

// Order entry over REST on both trees, with the lifecycle arriving on the
// matching user stream. A successful create emits nothing: it only seeds the
// memo a later fill reads its own id out of.
// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class BingxExec final : public exec::ExecutionClient {
public:
    BingxExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state_->signer}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {}

    ~BingxExec() override { state_->private_poll = nullptr; }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        sync_clock(*state_, rest_, unsigned_builder_);
        if (perp_) {
            read_position_mode();
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
            if (const auto request = builder_.listen_key_close(key_, state_->venue_now());
                request.has_value()) {
                (void)rest_.send(*request, [](net::RestResponse&&) {});
                state_->io.poll();
            }
        }
        perp_stream_.reset();
        spot_stream_.reset();
        key_.clear();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        if (!started_ || !(spot_ || perp_)) {
            return false;
        }
        if (perp_ && (perp_stream_ == nullptr || !perp_stream_->client.connected())) {
            return false;
        }
        if (spot_ && (spot_stream_ == nullptr || !spot_stream_->client.connected())) {
            return false;
        }
        return true;
    }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
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

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local OrderBuffer storage;
        const auto orders = parse_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            auto report = map_open_order(wire, state_->instruments, market);
            if (!report.has_value()) {
                continue; // another market's pair, or an instrument we never loaded
            }
            state_->remember(wire.order_id, report->exchange_client_order_id);
            reports.push_back(*report);
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        // Both single-order reads demand a SYMBOL and this call carries none,
        // so the open-order sweep answers the same question rather than an
        // endpoint that cannot hear it.
        const auto reports = open_order_reports(market);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        for (const auto& report : *reports) {
            if (report.exchange_client_order_id.view() == id.view()) {
                return report;
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // /openApi/spot/v1/trade/myTrades demands a symbol and BingX
            // offers no account-wide form; sweeping the pairs we happen to
            // have touched would answer "these are all of them" about a set
            // that is empty on every restart.
            return std::unexpected{
                exec::not_implemented("bingx spot fill pull (myTrades is per-symbol)")};
        }
        // /allFillOrders is account-wide but states no trade id and no side:
        // a FillReport without an exec id cannot be deduplicated and one
        // without a side cannot be netted. Live fills are unaffected —
        // ORDER_TRADE_UPDATE carries both.
        return std::unexpected{exec::not_implemented(
            "bingx perpetual fill pull (allFillOrders states no trade id and no side)")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local PositionBuffer storage;
        const auto positions = parse_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // A refusal arrives at HTTP 200 with the verdict in the body, so the status
    // is checked for transport health and the ENVELOPE decides the outcome.
    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "http error")};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable envelope")};
        }
        if (envelope->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }
        return response->body;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
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

    // Hedge and one-way need different parameters and BingX refuses the wrong
    // pair with 109400, so this is read once at start rather than guessed per
    // order. A read that fails leaves one-way, which is BingX's own default.
    void read_position_mode() {
        const auto request = builder_.position_mode(state_->venue_now());
        if (!request.has_value()) {
            return;
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return;
        }
        if (const auto hedge = parse_position_mode(*body); hedge.has_value()) {
            builder_.set_hedge_mode(*hedge);
        }
    }

    // Blocking, and only at start(): the sockets cannot connect without a key
    // and a cold start is the one place a round trip costs nothing. ONE key
    // serves both trees — the endpoint is account-level, not per-market.
    [[nodiscard]] bool mint_listen_key() {
        const auto request = builder_.listen_key_create(state_->venue_now());
        if (!request.has_value()) {
            state_->key_stale = true;
            return false;
        }
        const auto response = await_response(*state_, rest_, *request);
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
            perp_stream_->client.start();
        }
        if (spot_) {
            spot_stream_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot, key_);
            spot_stream_->client.start();
        }
    }

    // Asynchronous on purpose: this runs on the gateway's poll, where one
    // BingX timeout would stall every other venue for the length of it.
    void refresh_key() noexcept {
        if (key_in_flight_) {
            return;
        }
        const auto request = builder_.listen_key_create(state_->venue_now());
        if (!request.has_value()) {
            return; // no credentials: nothing to retry with
        }
        key_in_flight_ = true;
        if (!rest_.send(*request,
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
        const auto request = builder_.listen_key_keepalive(key_, state_->venue_now());
        if (!request.has_value()) {
            return;
        }
        keepalive_in_flight_ = true;
        if (!rest_.send(*request, [this](net::RestResponse&& response) {
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
    net::RestClient rest_;
    RequestBuilder unsigned_builder_;
    SignedRequestBuilder builder_;
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
