#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::htx::detail {

namespace {

constexpr std::uint32_t k_private_stream_id = 1;

// Every HTX socket but the spot v2 one gzips its frames, and libs/net hands on
// the bytes unchanged. A payload that carries no gzip magic is passed through
// rather than refused, so one handler shape serves both dialects.
std::optional<std::string_view>
decode_frame(net::Inflater& inflater, std::string_view payload, std::uint64_t& failures) {
    const auto inflated = inflater.inflate(payload);
    if (inflated.has_value()) {
        return *inflated;
    }
    if (inflated.error() == net::InflateError::NotGzip) {
        return payload;
    }
    ++failures;
    return std::nullopt;
}

// HTX's code is a string on spot and a number on the swap, and VenueError
// carries an integer, so the code travels in the message where an operator can
// still read it whichever spelling arrived.
exec::VenueError rest_failure(std::string_view body) {
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    if (envelope->ok) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "refusal body reports ok");
    }
    return exec::VenueError::make(classify(envelope->code), 0, envelope->msg);
}

exec::VenueError build_failure(BuildError err) noexcept {
    // A refusal to build is terminal by construction: the same command built
    // again produces the same refusal.
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// Emits the lifecycle event an order frame reports, and one Filled per trade
// the same frame carries. Shared by both private sockets and by the REST reads
// that answer with orders.
void publish_order(VenueState& state, const WireOrder& wire, UnixNanos received) {
    const auto leaves = order_leaves(wire, state.instruments);
    for (const auto& fill : wire.fills) {
        // Leaves after THIS frame's trades is the best the swap states; a
        // per-trade running total would have to assume the array is ordered.
        if (const auto event = map_fill(
                wire, fill, state.instruments, leaves.value_or(QtyLots::from_raw(0)), received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
    if (const auto event = map_order_event(wire, state.instruments, received); event.has_value()) {
        state.sink->on_order_event(*event);
    }
    state.remember(wire);
}

// The USDT-M swap's private socket. Same host, path and gzip as the funding
// notification stream — HTX serves public and private topics from one
// endpoint — but this one signs a handshake first, so it is its own connection.
class SwapPrivateHandler final : public net::WsHandler {
public:
    explicit SwapPrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto frame = state_->protocol.swap_auth_frame(state_->signer, state_->venue_now());
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = decode_frame(inflater_, payload, state_->inflate_failures);
            if (!frame.has_value()) {
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return;
            }
            switch (event->kind) {
            case EventKind::NotifyPing:
                if (const auto pong = state_->protocol.notify_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
                return;
            case EventKind::AuthReply:
                on_auth(*frame);
                return;
            case EventKind::PrivateOrder: {
                static thread_local OrderBuffer orders;
                if (const auto wire = parse_swap_order_push(*frame, orders); wire.has_value()) {
                    publish_order(*state_, *wire, received);
                }
                return;
            }
            default:
                return; // acks and position pushes carry nothing we act on
            }
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void on_auth(std::string_view frame) {
        const auto reply = parse_auth_reply(frame);
        if (!reply.has_value() || !reply->ok) {
            state_->last_error_code = reply.has_value() ? std::string{reply->code} : std::string{};
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        // matchOrders_cross is deliberately NOT subscribed: it repeats the very
        // trades orders_cross already carries inline, and without trade_fee.
        // Emitting from both would attribute one fill to two events.
        const auto frames = std::array<std::string_view, 1>{k_swap_orders_topic};
        for (const auto& topic : frames) {
            const auto sub = state_->protocol.swap_private_subscribe_frame(topic);
            if (!sub.has_value() || !client_->send(sub->text)) {
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = k_private_stream_id,
                                               .ts = state_->clock->wall()});
                return;
            }
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

// The spot private socket: a different host, a different dialect, and — alone
// among HTX's sockets — plain text rather than gzip. Its orders channel states
// what an order has LEFT but never a fee, and trade.clearing states the fee but
// is the only place the exec id exists, so a fill needs both.
class SpotPrivateHandler final : public net::WsHandler {
public:
    explicit SpotPrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto frame = state_->protocol.spot_auth_frame(state_->signer, state_->venue_now());
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = decode_frame(inflater_, payload, state_->inflate_failures);
            if (!frame.has_value()) {
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return;
            }
            switch (event->kind) {
            case EventKind::SpotPing:
                if (const auto pong = state_->protocol.spot_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
                return;
            case EventKind::AuthReply:
                on_auth(*frame);
                return;
            case EventKind::PrivateOrder:
                if (const auto wire = parse_spot_order_push(*frame); wire.has_value()) {
                    publish_order(*state_, *wire, received);
                }
                return;
            case EventKind::PrivateMatch:
                on_trade(*frame, received);
                return;
            default:
                return;
            }
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void on_auth(std::string_view frame) {
        const auto reply = parse_auth_reply(frame);
        if (!reply.has_value() || !reply->ok) {
            state_->last_error_code = reply.has_value() ? std::string{reply->code} : std::string{};
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        const auto channels =
            std::array<std::string_view, 2>{k_spot_orders_channel, k_spot_trades_channel};
        for (const auto& channel : channels) {
            const auto sub = state_->protocol.spot_private_subscribe_frame(channel);
            if (!sub.has_value() || !client_->send(sub->text)) {
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = k_private_stream_id,
                                               .ts = state_->clock->wall()});
                return;
            }
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_trade(std::string_view frame, UnixNanos received) {
        const auto wire = parse_spot_trade_push(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto memo = state_->memo_for(wire->order_id);
        if (const auto event = map_user_trade(*wire,
                                              state_->instruments,
                                              memo.leaves,
                                              memo.client_order_id,
                                              memo.exchange_client_order_id,
                                              received);
            event.has_value()) {
            state_->sink->on_order_event(*event);
        }
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

// One private socket and the client that drives it. Spot and the swap are
// different hosts carrying different dialects, so an account trading both holds
// two of these.
template <typename Handler> struct PrivateStream {
    PrivateStream(VenueState& state, std::string_view host, std::string_view path)
        : handler{state}, client{state.io,
                                 net::WsConfig{.host = std::string{host},
                                               .port = std::string{stream_port()},
                                               .target = std::string{path}},
                                 handler} {
        handler.attach(client);
    }

    Handler handler;
    net::WsClient client;
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class HtxExec final : public exec::ExecutionClient {
public:
    HtxExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)},
          perp_rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{spot_rest_host()}, .port = "443"}},
          builder_{state_->signer}, spot_enabled_{market == MarketType::Spot} {
        if (market == MarketType::Perp) {
            perp_stream_ = std::make_unique<PrivateStream<SwapPrivateHandler>>(
                *state_, stream_host(), notify_path());
        }
        if (spot_enabled_) {
            spot_stream_ = std::make_unique<PrivateStream<SpotPrivateHandler>>(
                *state_, spot_stream_host(), spot_private_path());
        }
    }

    std::expected<void, exec::VenueError> start() override {
        perp_rest_.start();
        spot_rest_.start();
        sync_clock(*state_, perp_rest_);
        if (spot_enabled_ && !load_spot_account()) {
            // Without it every spot order is refused as an invalid parameter,
            // one at a time, which reads as a broken instrument table.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "htx: spot account id unavailable")};
        }
        if (perp_stream_) {
            perp_stream_->client.start();
        }
        if (spot_stream_) {
            spot_stream_->client.start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (perp_stream_) {
            perp_stream_->client.stop();
        }
        if (spot_stream_) {
            spot_stream_->client.stop();
        }
        perp_rest_.stop();
        spot_rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        const bool perp_up = perp_stream_ == nullptr || perp_stream_->client.connected();
        const bool spot_up = spot_stream_ == nullptr || spot_stream_->client.connected();
        return started_ && perp_up && spot_up &&
               (perp_stream_ != nullptr || spot_stream_ != nullptr);
    }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        perp_rest_.keepalive(now);
        spot_rest_.keepalive(now);
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

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, 1, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local OrderBuffer storage;
        const auto orders = parse_rest_orders(*body, storage, market);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            state_->remember(wire);
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
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
        std::int64_t numeric = 0;
        std::string_view contract;
        if (market != MarketType::Spot) {
            const auto it = state_->swap_targets.find(std::string{id.view()});
            if (it == state_->swap_targets.end()) {
                // The swap never saw this ULID: its client_order_id is an
                // int64 and the pairing lives only in this process. Saying so
                // beats answering about an order we cannot name.
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, "htx swap: client order id not minted here")};
            }
            numeric = it->second.client_order_id;
            contract = it->second.contract_code;
        }
        const auto request =
            builder_.query_order(id, numeric, contract, market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local OrderBuffer storage;
        const auto orders = parse_rest_orders(*body, storage, market);
        if (!orders.has_value() || orders->empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not readable")};
        }
        auto report = map_open_order(orders->front(), state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        (void)since;
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        // Both surfaces window their trade history by CONTRACT and this call
        // names only a market. GET /v1/order/matchresults requires `symbol`,
        // and swap_cross_matchresults answers err_code 1014 without a
        // contract_code or a pair. A sweep over the instruments we happen to
        // know about would be a guess, and an empty vector reads as "no fills"
        // — the one answer a reconciler must not be handed wrongly.
        return std::unexpected{exec::not_implemented("htx fill pull is per-contract")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // A spot balance is not a position, and one BTC balance backs
            // btcusdt, btcusdc and btceth alike. Answering "flat" would be a
            // claim about a concept HTX does not have on this API.
            return std::unexpected{exec::not_implemented("htx spot position pull")};
        }
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
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

    [[nodiscard]] net::RestClient& rest_for(MarketType market) noexcept {
        return market == MarketType::Spot ? spot_rest_ : perp_rest_;
    }

    [[nodiscard]] bool load_spot_account() {
        if (builder_.spot_account_id() != 0) {
            return true;
        }
        const auto request = builder_.spot_accounts(state_->venue_now());
        if (!request.has_value()) {
            return false;
        }
        const auto body = fetch(*request, MarketType::Spot);
        if (!body.has_value()) {
            return false;
        }
        const auto id = parse_spot_account_id(*body);
        if (!id.has_value() ||
            *id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        builder_.set_spot_account_id(static_cast<std::int64_t>(*id));
        return true;
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request, MarketType market) {
        const auto response = await_response(*state_, rest_for(market), request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "http error")};
        }
        // HTTP 200 with status "error" is HTX's ordinary refusal on both
        // surfaces, so the envelope decides rather than the status line.
        const auto envelope = parse_rest_envelope(response->body);
        if (envelope.has_value() && !envelope->ok) {
            return std::unexpected{rest_failure(response->body)};
        }
        return response->body;
    }

    void send_batch(std::span<const OrderCmd> group) noexcept {
        if (group.empty()) {
            return;
        }
        const auto market = group.front().instrument.market();
        const auto request = builder_.cancel_batch(group, state_->instruments, state_->venue_now());
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
        if (!rest_for(market).send(*request, [this, pending](net::RestResponse&& response) {
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
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        const auto market = cmd.instrument.market();
        if (kind == Kind::Create && market != MarketType::Spot) {
            remember_swap_target(cmd);
        }
        if (!rest_for(market).send(*request,
                                   [this, cmd, kind, market](net::RestResponse&& response) {
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
    net::RestClient perp_rest_;
    net::RestClient spot_rest_;
    SignedRequestBuilder builder_;
    bool spot_enabled_{false};
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
