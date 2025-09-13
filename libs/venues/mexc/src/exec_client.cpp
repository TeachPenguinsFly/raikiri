#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/positions.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::mexc::detail {

namespace {

constexpr std::uint32_t k_private_stream_id = 1;

// Routes one futures personal push. MEXC sends these unbidden after a login,
// so the socket carrying them is authenticated, not subscribed.
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::PersonalOrder: {
        const auto wire = parse_futures_order(frame);
        if (!wire.has_value()) {
            return;
        }
        // Remembered BEFORE the event is emitted: a deal for the same fill may
        // already be queued behind this frame, and it can only state its
        // leaves out of this memo.
        state.remember(*wire);
        if (const auto mapped = map_futures_order_event(*wire, state.instruments, received);
            mapped.has_value()) {
            state.sink->on_order_event(*mapped);
        }
        break;
    }
    case EventKind::PersonalDeal: {
        const auto wire = parse_futures_deal(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto memo = state.memo_for(wire->order_id);
        if (const auto mapped = map_futures_deal(
                *wire, state.instruments, memo.leaves, memo.client_order_id, received);
            mapped.has_value()) {
            state.sink->on_order_event(*mapped);
        }
        break;
    }
    case EventKind::PersonalPosition:
        // GatewaySink carries no position event; the reconciler asks for
        // positions through position_reports() instead.
        break;
    case EventKind::LoginAck:
        state.sink->on_connectivity({.state = exec::ConnState::Connected,
                                     .stream_id = k_private_stream_id,
                                     .ts = state.clock->wall()});
        break;
    case EventKind::Error: {
        // rs.error carries prose and no code at all, so a rejected login and a
        // rejected subscription are indistinguishable here; the socket is only
        // ever used for a login, which is what makes AuthFailed the reading.
        if (parse_stream_error(frame).has_value()) {
            state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                         .stream_id = k_private_stream_id,
                                         .ts = state.clock->wall()});
        }
        break;
    }
    case EventKind::Ticker:
    case EventKind::Trade:
    case EventKind::Depth:
    case EventKind::DepthFull:
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

exec::VenueError build_failure(BuildError err) noexcept {
    // A refusal to build is terminal by construction: the same command built
    // again produces the same refusal.
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. An empty answer here would read as "the venue holds nothing".
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("mexc dated futures order entry");
    case MarketType::Option:
        // MEXC lists no options on either API surface, so nothing maps an
        // OptionInstrument onto one.
        return exec::not_implemented("mexc options order entry");
    }
    return exec::not_implemented("mexc: unknown market");
}

// The futures private socket. It is a SECOND connection to the same /edge
// endpoint the public one uses: MEXC authenticates the socket rather than
// minting a listen key, and the personal pushes follow the login unbidden.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto frame =
            state_->protocol.login_frame(state_->signer, state_->venue_now(MarketType::Perp));
        if (!frame.has_value() || client_ == nullptr || !client_->send(*frame)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

struct PrivateStream {
    explicit PrivateStream(VenueState& state)
        : handler{state}, client{state.io,
                                 net::WsConfig{.host = std::string{stream_host(state.market)},
                                               .port = std::string{stream_port()},
                                               .target = std::string{stream_path(state.market)}},
                                 handler} {
        handler.attach(client);
    }

    PrivateHandler handler;
    net::WsClient client;
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Order entry over REST for both markets.
//
// MEXC has historically gated futures order placement behind an account
// permission: the legacy contract API closed its place/cancel endpoints in
// July 2022 and MEXC reopened API futures trading on 2026-03-31 behind KYC
// plus an explicit "Order Placing" grant on the key. A key without that grant
// answers a permission code (701-704), which classifies as AuthFailure — the
// order is refused loudly rather than left open.
class MexcExec final : public exec::ExecutionClient {
public:
    MexcExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{private_rest_host()}, .port = "443"}},
          builder_{state_->signer}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {
        if (perp_) {
            stream_ = std::make_unique<PrivateStream>(*state_);
            state_->private_client = &stream_->client;
        }
    }

    ~MexcExec() override {
        if (state_->private_client == (stream_ ? &stream_->client : nullptr)) {
            state_->private_client = nullptr;
        }
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        if (perp_) {
            sync_clock(*state_, rest_);
        }
        if (spot_) {
            sync_spot_clock(*state_, rest_);
        }
        if (stream_) {
            stream_->client.start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (stream_) {
            stream_->client.stop();
        }
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        // Spot has no JSON user stream to be up or down, so an account trading
        // spot alone is connected once its REST pool is.
        const bool stream_up = stream_ == nullptr || stream_->client.connected();
        return started_ && stream_up && (spot_ || perp_);
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
            const auto request = builder_.cancel_batch(
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
            if (!rest_.send(*request, [this, pending](net::RestResponse&& response) {
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

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // /api/v3/openOrders demands a symbol and MEXC offers no
            // account-wide form. Sweeping the pairs we happen to have touched
            // would answer "these are all of them" about a set that is empty
            // on every restart.
            return std::unexpected{
                exec::not_implemented("mexc spot open-order sweep (openOrders is per-symbol)")};
        }
        const auto request = builder_.open_orders(market, {}, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local FuturesOrderBuffer storage;
        const auto orders = parse_futures_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_futures_open_order(wire, state_->instruments);
                report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Perp) {
            // The futures single-order read is keyed by symbol AND externalOid
            // and this call carries no symbol, so the open-order sweep answers
            // the same question.
            return from_open_orders(id, market);
        }
        const auto request = builder_.query_order(id, market, {}, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_spot_order(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto report = map_spot_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // /api/v3/myTrades is per-symbol with no account-wide form; an
            // empty vector here would read as "nothing traded".
            return std::unexpected{
                exec::not_implemented("mexc spot fill pull (myTrades is per-symbol)")};
        }
        const auto request = builder_.my_trades(market, {}, since, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local FuturesDealBuffer storage;
        const auto deals = parse_futures_deals(*body, storage);
        if (!deals.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(deals.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(deals->size());
        for (const auto& wire : *deals) {
            const auto memo = state_->memo_for(wire.order_id);
            const auto event = map_futures_deal(wire,
                                                state_->instruments,
                                                memo.leaves,
                                                memo.client_order_id,
                                                state_->clock->wall());
            if (!event.has_value()) {
                continue;
            }
            reports.push_back({.exec_id = event->exec_id,
                               .exchange_order_id = event->exchange_order_id,
                               .exchange_client_order_id = event->exchange_client_order_id,
                               .instrument = event->instrument,
                               .side = event->side,
                               .price = event->price,
                               .qty = event->qty,
                               .fee = event->fee,
                               .liquidity = event->liquidity,
                               .ts_venue = event->ts_event});
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // A spot balance is not a position. Answering "flat" would be a
            // claim about a concept this API does not have.
            return std::unexpected{exec::not_implemented("mexc spot position pull")};
        }
        const auto request = builder_.positions(market, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local FuturesPositionBuffer storage;
        const auto positions = parse_futures_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_futures_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request, MarketType market) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body, market)};
        }
        // Futures answers 200 with success:false, so the status alone is not
        // the verdict on that tree.
        if (market != MarketType::Spot) {
            const auto envelope = parse_rest_envelope(response->body);
            if (!envelope.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable envelope")};
            }
            if (!envelope->success || envelope->code != k_ok) {
                return std::unexpected{exec::VenueError::make(
                    classify(envelope->code), envelope->code, envelope->msg)};
            }
        }
        return response->body;
    }

    [[nodiscard]] std::expected<exec::OrderStatusReport, exec::VenueError>
    from_open_orders(const ExchangeClientOrderId& id, MarketType market) {
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

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto market = cmd.instrument.market();
        const auto now = state_->venue_now(market);
        const auto request =
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!rest_.send(*request, [this, cmd, kind, market](net::RestResponse&& response) {
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
    net::RestClient rest_;
    SignedRequestBuilder builder_;
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

} // namespace raikiri::venues::mexc::detail
