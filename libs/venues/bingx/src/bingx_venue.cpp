#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bingx/client_id.hpp>
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

namespace raikiri::venues::bingx {

namespace {

// Funding is a poll, not a stream (see wire.hpp). One symbol goes out per tick,
// so the request rate is bounded by the cadence rather than by how many
// contracts the gateway watches.
constexpr std::uint64_t k_funding_poll_interval_ns = 2'000'000'000ULL;

// A listen key is valid one hour and BingX recommends extending every thirty
// minutes. Twenty leaves room for a missed tick and a failed extension inside
// the window rather than discovering the key is dead from a listenKeyExpired
// push with orders working.
constexpr std::uint64_t k_listen_key_interval_ns = 20ULL * 60 * 1'000'000'000ULL;

// Live orders tracked for our own id. Far above any sane working set; the cap
// exists so a venue incident cannot turn the map into the heap.
constexpr std::size_t k_max_live_orders = 8'192;

constexpr std::uint32_t k_perp_stream_id = 1;
constexpr std::uint32_t k_spot_stream_id = 2;

// The spot user stream is the one that needs asking; the perpetual one pushes
// everything as soon as the listenKey connects.
constexpr std::string_view k_spot_user_channel = "spot.executionReport";

// Owned as strings: the instrument table's views outlive nothing in
// particular.
struct OwnedSubscription {
    std::string symbol;
    std::string suffix;
};

// State every bingx connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, so nothing here
// synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;

    // The one market this process serves. BingX puts the two on separate
    // deployments with different frame shapes, so almost everything below
    // reads it: the host, the path, the dataType, and how a depth frame is
    // ordered.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    net::Signer signer;

    std::vector<OwnedSubscription> desired;

    // Symbols the gateway asked for funding on, polled round-robin.
    std::vector<std::string> funding_symbols;
    std::size_t funding_cursor{0};
    std::uint64_t next_funding_poll_ns{0};

    // Venue clock offset from /server/time. One host serves both trees, so one
    // offset is right for both — unlike xt, where two deployments drift apart.
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::uint64_t inflate_failures{0};

    // The execution client's per-tick work: ExecutionClient has no poll hook
    // of its own, and a listen key expires whether or not anyone is looking.
    std::function<void(UnixNanos)> private_poll;

    // Set when BingX says the key is dead. The socket cannot recover without a
    // fresh one, and the key rides the URL, so recovery is a reconnect.
    bool key_stale{false};

    // Our own id for a venue order id. The SPOT push carries no client order
    // id at all, so without this every spot fill would be unattributable —
    // and a restart could not tell our orders from a web order's.
    std::unordered_map<std::string, ExchangeClientOrderId> live_orders;

    void add_subscription(std::string_view symbol, std::string_view suffix) {
        for (const auto& existing : desired) {
            if (existing.symbol == symbol && existing.suffix == suffix) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{symbol}, std::string{suffix});
    }

    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(std::string_view order_id, const ExchangeClientOrderId& coid) {
        if (order_id.empty() || coid.empty()) {
            return;
        }
        auto key = std::string{order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        live_orders[std::move(key)] = coid;
    }

    void forget(std::string_view order_id) { live_orders.erase(std::string{order_id}); }

    [[nodiscard]] ExchangeClientOrderId remembered(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? ExchangeClientOrderId{} : it->second;
    }
};

void handle_frame(VenueState& state, EventKind kind, std::string_view frame, UnixNanos received) {
    switch (kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, state.market, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
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
            if (const auto trade = map_trade(entry, state.instruments, state.market, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, state.market, received);
            book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto control = parse_control(frame); control.has_value()) {
            state.last_error_code = control->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarkPrice:
    case EventKind::Ping:
    case EventKind::ControlAck:
    case EventKind::Unknown:
        break;
    }
}

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

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& entry : state_->desired) {
            const auto frame = state_->protocol.subscribe_frame(entry.symbol, entry.suffix);
            if (!frame.has_value()) {
                continue;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = inflater_.inflate(payload);
            if (!frame.has_value()) {
                ++state_->inflate_failures;
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return; // unknown shapes are ignored, never fatal
            }
            if (event->kind == EventKind::Ping) {
                (void)client_->send(Protocol::pong_frame());
                return;
            }
            handle_frame(*state_, event->kind, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

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

void sync_clock(VenueState& state, net::RestClient& rest, RequestBuilder& builder) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, builder.server_time(), 5'000);
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

class BingxData final : public exec::DataClient {
public:
    explicit BingxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->market)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path(state_->market)}},
                  handler_},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        client_.start();
        rest_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // This socket is one deployment's, and BingX spells a spot pair
            // and its perpetual identically — an id from the other market
            // subscribed here would be fed the wrong tree's prints under the
            // right-looking symbol, which is worse than no data.
            return std::unexpected{exec::not_implemented("bingx: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            // Not a stream: BingX publishes funding over REST only.
            for (const auto& existing : state_->funding_symbols) {
                if (existing == symbol) {
                    return {};
                }
            }
            state_->funding_symbols.emplace_back(symbol);
            return {};
        }
        const auto suffix = data_type_suffix(kind, state_->market);
        if (suffix.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        state_->add_subscription(symbol, suffix);
        if (client_.connected()) {
            if (const auto frame = state_->protocol.subscribe_frame(symbol, suffix);
                frame.has_value()) {
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
        poll_funding(now);
        // One io context serves both clients, and this is the exec client's
        // only tick.
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_symbols.empty() || now.ns < state_->next_funding_poll_ns ||
            funding_in_flight_) {
            return;
        }
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
        const auto& symbol = state_->funding_symbols[state_->funding_cursor];
        state_->funding_cursor = (state_->funding_cursor + 1) % state_->funding_symbols.size();
        funding_in_flight_ = true;
        try {
            if (!rest_.send(builder_.premium_index(symbol),
                            [this](const net::RestResponse& response) { on_funding(response); })) {
                funding_in_flight_ = false;
            }
        } catch (...) {
            funding_in_flight_ = false;
        }
    }

    void on_funding(const net::RestResponse& response) noexcept {
        funding_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return; // the next tick retries; a missed rate is not an incident
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value() || envelope->code != k_ok) {
            return;
        }
        const auto wire = parse_premium_index(response.body);
        if (!wire.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        if (const auto funding = map_funding(*wire, state_->instruments, received);
            funding.has_value()) {
            state_->sink->on_funding(*funding);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    net::RestClient rest_;
    RequestBuilder builder_;
    bool funding_in_flight_{false};
    bool started_{false};
};

// Order entry over REST on both trees, with the lifecycle arriving on the
// matching user stream. A successful create emits nothing: it only seeds the
// memo a later fill reads its own id out of.
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
    void poll(UnixNanos now) noexcept override { rest_.keepalive(now); }

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
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        if (response.transport_failed()) {
            emit_reject(cmd,
                        kind, // no verdict from the venue at all
                        exec::VenueError::make(
                            exec::ErrorClass::Connectivity, 0, "no response from venue"));
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
};

class BingxInstruments final : public exec::InstrumentSource {
public:
    explicit BingxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_, builder_);

        switch (market) {
        case MarketType::Perp:
            return load_perp();
        case MarketType::Spot:
            return load_spot();
        case MarketType::Future:
        case MarketType::Option:
            // Neither tree lists them, so an empty vector would be a claim
            // about a market BingX does not have.
            return std::unexpected{exec::not_implemented(to_string(*market_refusal(market)))};
        }
        return std::unexpected{exec::not_implemented("bingx: unknown market")};
    }

private:
    [[nodiscard]] std::expected<std::string, exec::VenueError> read(const net::RestRequest& request,
                                                                    std::uint64_t timeout_ms) {
        const auto response = await_response(*state_, rest_, request, timeout_ms);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "catalogue: http error")};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (envelope->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }
        return response->body;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        const auto body = read(builder_.contracts(), 20'000);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(
            *wire, [](const WireContract& row) { return map_instrument(row, MarketType::Perp); });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        const auto body = read(builder_.spot_symbols(), 30'000);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local SpotSymbolBuffer storage;
        const auto wire = parse_spot_symbols(*body, storage);
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
                continue; // not online, closed to the API, or missing a step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                // BingX spells a spot pair and a perpetual IDENTICALLY, so one
                // table holds whichever market loaded first. Reported by count
                // rather than by throw, as a duplicate or a full table is.
                continue;
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    RequestBuilder builder_;
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
    state->market = config.market;
    adapter.caps = exec::caps_of(VenueId::Bingx);
    adapter.data = std::make_unique<BingxData>(state);
    adapter.execution = std::make_unique<BingxExec>(state, config.market);
    adapter.instruments = std::make_unique<BingxInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bingx, &make);
}

} // namespace raikiri::venues::bingx
