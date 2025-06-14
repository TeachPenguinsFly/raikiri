#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::mexc {

namespace {

// MEXC closes a socket that has sent nothing for 60 s and counts only CLIENT
// messages toward that timer — verified: a connection receiving a push every
// three seconds was still dropped at 62 s. Ten seconds leaves room for a
// missed ping and a reconnect inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 10'000'000'000ULL;

// Live orders tracked for their leaves. Far above any sane working set; the
// cap exists so a venue incident cannot turn the map into the heap.
constexpr std::size_t k_max_live_orders = 8'192;

constexpr std::uint32_t k_private_stream_id = 1;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string symbol;
    std::uint32_t limit{0};
};

// State every mexc connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    // The execution client's socket, registered so the data client's poll can
    // keep it alive too. ExecutionClient has no poll hook and MEXC's 60 s idle
    // timer counts only CLIENT messages — a socket that only receives is
    // closed — so the one timer this adapter has drives both connections.
    net::WsClient* private_client{nullptr};

    // Venue clock offsets, measured separately per tree: /api/v1 and /api/v3
    // check a request against their own clock, and one offset for both is a
    // signature that expires on whichever host drifted.
    std::int64_t clock_offset_ns{0};
    std::int64_t spot_clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};

    // As much of a live futures order as MEXC states in any one frame. The
    // order push carries `vol`/`dealVol` and our externalOid; the deal push
    // carries the fill and neither, so a fill cannot state its own leaves —
    // which is the number the OMS treats as authoritative. Keyed by the venue
    // order id, the one field both frames do carry.
    struct OrderMemo {
        ExchangeClientOrderId client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };
    std::unordered_map<std::string, OrderMemo> live_orders;

    void add_subscription(std::string_view channel, std::string_view symbol, std::uint32_t limit) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{symbol}, limit);
    }

    [[nodiscard]] UnixNanos venue_now(MarketType market) const noexcept {
        const auto offset = market == MarketType::Spot ? spot_clock_offset_ns : clock_offset_ns;
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + offset)};
    }

    void remember(const WireFuturesOrder& wire) {
        if (wire.order_id.empty()) {
            return;
        }
        // 3 filled, 4 cancelled, 5 invalid: nothing further can reference it.
        if (wire.state >= 3) {
            live_orders.erase(std::string{wire.order_id});
            return;
        }
        const auto* instrument = instruments.find(wire.symbol);
        if (instrument == nullptr) {
            return;
        }
        auto key = std::string{wire.order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        auto& memo = live_orders[key];
        if (const auto report = map_futures_open_order(wire, instruments); report.has_value()) {
            memo.leaves = report->leaves;
            if (!report->exchange_client_order_id.empty()) {
                memo.client_order_id = report->exchange_client_order_id;
            }
        }
    }

    [[nodiscard]] OrderMemo memo_for(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? OrderMemo{} : it->second;
    }
};

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::DepthFull: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Depth:
        break;
    case EventKind::Error: {
        if (parse_stream_error(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::PersonalOrder:
    case EventKind::PersonalDeal:
    case EventKind::PersonalPosition:
    case EventKind::LoginAck:
        break; // private kinds belong to the execution client's own socket
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

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

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& entry : state_->desired) {
            const std::array<Subscription, 1> one{Subscription{
                .channel = entry.channel, .symbol = entry.symbol, .limit = entry.limit}};
            const auto frame = state_->protocol.subscribe_frame(one);
            if (!frame.has_value() || !client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class MexcData final : public exec::DataClient {
public:
    explicit MexcData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host()},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != MarketType::Perp) {
            // contract.mexc.com is the only public host this adapter speaks,
            // and it carries futures alone. MEXC spot order entry works over
            // api.mexc.com, so a spot gateway can trade and see nothing —
            // saying so here is what keeps that from looking like a quiet
            // market.
            return std::unexpected{exec::not_implemented("mexc spot market data")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        const std::uint32_t limit = kind == exec::SubKind::Bbo ? k_book_depth : 0;
        state_->add_subscription(channel, symbol, limit);
        if (client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.channel = channel, .symbol = symbol, .limit = limit}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        if (now.ns < state_->next_app_ping_ns) {
            return;
        }
        if (client_.connected()) {
            (void)client_.send(Protocol::ping_frame());
        }
        if (state_->private_client != nullptr && state_->private_client->connected()) {
            (void)state_->private_client->send(Protocol::ping_frame());
        }
        state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

std::optional<net::RestResponse> await_response(VenueState& state,
                                                net::RestClient& rest,
                                                const net::RestRequest& request,
                                                std::uint64_t timeout_ms = 10'000) {
    std::optional<net::RestResponse> result;
    if (!rest.send(request,
                   [&result](net::RestResponse&& response) { result = std::move(response); })) {
        return std::nullopt;
    }
    const auto deadline = state.clock->mono().ns + timeout_ms * 1'000'000ULL;
    while (!result.has_value() && state.clock->mono().ns < deadline) {
        if (state.io.poll() == 0) {
            state.io.run_for(5);
        }
    }
    return result;
}

void sync_clock(VenueState& state, net::RestClient& rest) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, state.builder.server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return;
    }
    const auto venue_ms = parse_server_time(response->body);
    if (!venue_ms.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

void sync_spot_clock(VenueState& state, net::RestClient& rest) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, state.builder.spot_server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return;
    }
    const auto venue_ms = parse_spot_server_time(response->body);
    if (!venue_ms.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.spot_clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
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

// The two trees report a refusal differently: futures wraps it in
// {"success":false,"code":..,"message":..} and spot answers a bare
// {"code":..,"msg":..}. Reading one with the other's parser loses the code,
// and the code is what decides retry from halt.
exec::VenueError rest_failure(std::string_view body, MarketType market) {
    if (market == MarketType::Spot) {
        const auto err = parse_spot_error(body);
        if (!err.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
        }
        return exec::VenueError::make(classify_spot(err->code), err->code, err->msg);
    }
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    return exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg);
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
                                 net::WsConfig{.host = std::string{stream_host()},
                                               .port = std::string{stream_port()},
                                               .target = std::string{stream_path()}},
                                 handler} {
        handler.attach(client);
    }

    PrivateHandler handler;
    net::WsClient client;
};

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
    MexcExec(std::shared_ptr<VenueState> state, bool spot, bool perp)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{private_rest_host()}, .port = "443"}},
          builder_{state_->signer}, spot_{spot}, perp_{perp} {
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
            const auto request =
                builder_.cancel_batch(chunk, state_->instruments, state_->venue_now(MarketType::Perp));
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
                    for (const auto& cmd : pending) {
                        emit_reject(cmd, Kind::Cancel);
                    }
                })) {
                for (const auto& cmd : chunk) {
                    emit_reject(cmd, Kind::Cancel);
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
        const auto request =
            builder_.open_orders(market, {}, state_->venue_now(market));
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
            if (auto report = map_futures_position(wire, state_->instruments);
                report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    [[nodiscard]] std::expected<std::string, exec::VenueError> fetch(const net::RestRequest& request,
                                                                    MarketType market) {
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
            emit_reject(cmd, kind);
            return;
        }
        if (!rest_.send(*request, [this, cmd, kind, market](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind, market);
            })) {
            emit_reject(cmd, kind);
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind,
                     MarketType market) {
        if (response.transport_failed()) {
            emit_reject(cmd, kind); // no verdict from the venue at all
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
                emit_reject(cmd, kind);
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
        const bool accepted = http_ok && envelope.has_value() && envelope->success &&
                              envelope->code == k_ok;
        if (accepted) {
            return; // the socket's push carries the lifecycle
        }
        if (kind == Kind::Cancel && envelope.has_value() &&
            is_benign_cancel_failure(envelope->code)) {
            return;
        }
        emit_reject(cmd, kind);
    }

    void emit_reject(const OrderCmd& cmd, Kind kind) noexcept {
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
};

class MexcInstruments final : public exec::InstrumentSource {
public:
    explicit MexcInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{private_rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        switch (market) {
        case MarketType::Perp:
            return load_perp();
        case MarketType::Spot:
            return load_spot();
        case MarketType::Future:
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "mexc lists no dated futures")};
        case MarketType::Option:
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "mexc lists no options")};
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "mexc: unknown market")};
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response =
            await_response(*state_, rest_, state_->builder.contract_detail(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (!envelope->success || envelope->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_instruments(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, [](const WireInstrument& row) {
            return map_instrument(row, MarketType::Perp);
        });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        spot_rest_.start();
        sync_spot_clock(*state_, spot_rest_);

        const auto response =
            await_response(*state_, spot_rest_, state_->builder.exchange_info(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body, MarketType::Spot)};
        }

        static thread_local SpotSymbolBuffer storage;
        const auto wire = parse_spot_symbols(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, map_spot_instrument);
    }

    template <typename Row, typename Map>
    std::vector<Instrument> intern(std::span<const Row> rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(state_->instruments.size() + rows.size());
        for (const auto& row : rows) {
            auto instrument = map(row);
            if (!instrument.has_value()) {
                continue; // not enabled, or a scale finer than FixedE9 models
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
};

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);

    config::DefaultSecretProvider provider;
    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state->signer.set_api_key(key.view());
            }
        }
    }
    if (!config.api_secret_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            (void)provider.resolve(*ref, state->signer.secret());
        }
    }

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Mexc);
    adapter.data = std::make_unique<MexcData>(state);
    adapter.execution = std::make_unique<MexcExec>(state, config.enable_spot, config.enable_perp);
    adapter.instruments = std::make_unique<MexcInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Mexc, &make);
}

} // namespace raikiri::venues::mexc
