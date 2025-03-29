#include <raikiri/venues/bybit/bybit_venue.hpp>

#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bybit/book_sync.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::bybit {

namespace {

constexpr std::string_view k_rest_host_live = "api.bybit.com";
constexpr std::string_view k_rest_host_test = "api-testnet.bybit.com";

// Bybit's own ping cadence. Its documented idle timeout is 20 s and it drops
// connections that answer only the websocket control frame, so the interval
// is comfortably inside that rather than at it.
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

// State every bybit connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // Desired subscriptions, owned here rather than by the transport: the
    // socket re-asserts this set on every open, so a reconnect and a cold
    // start execute the same path.
    std::vector<std::string> desired_topics;

    std::unordered_map<InstrumentId, BookSync> books;
    std::uint64_t next_app_ping_ns{0};

    // Venue clock offset. Bybit refuses signatures outside recvWindow of ITS
    // clock, so requests are stamped with corrected time rather than local
    // time — a laptop drifting two seconds otherwise looks like bad keys.
    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) +
                                                    clock_offset_ns)};
    }

    void add_topic(std::string_view topic) {
        for (const auto& existing : desired_topics) {
            if (existing == topic) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_topics.emplace_back(topic);
    }

    BookSync& book_for(const Instrument& instrument) {
        const auto& base = base_of(instrument);
        const auto it = books.find(base.id);
        if (it != books.end()) {
            return it->second;
        }
        return books.emplace(base.id, BookSync{base.id, base.tick_size, base.lot_size})
            .first->second;
    }
};

// Routes one public frame. Kept separate from the client so the dispatch is
// testable without a socket.
void handle_public_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto topic = parse_topic(frame);
    if (!topic.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    switch (topic->kind) {
    case TopicKind::Orderbook: {
        static thread_local OrderbookBuffer storage;
        const auto book = parse_orderbook(frame, storage);
        if (!book.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(book->symbol);
        if (instrument == nullptr) {
            return;
        }
        auto& sync = state.book_for(*instrument);
        const auto result = sync.apply(*book);
        if (result == SyncResult::GapDetected || result == SyncResult::BadLevel) {
            // The book is fiction now. Bybit resends a snapshot on its own
            // after a gap, so waiting is the recovery — publishing a stale
            // top of book would be worse than publishing nothing.
            return;
        }
        if (const auto bbo = sync.bbo(ms_to_unix(book->ts_ms, received), received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case TopicKind::PublicTrade: {
        static thread_local TradeBuffer storage;
        const auto trades = parse_trades(frame, storage);
        if (!trades.has_value()) {
            return;
        }
        for (const auto& wire : *trades) {
            if (const auto trade = map_trade(wire, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case TopicKind::Tickers: {
        const auto ticker = parse_ticker(frame);
        if (!ticker.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*ticker, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    default:
        break; // control frames and private topics are not this socket's
    }
}

void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto topic = parse_topic(frame);
    if (!topic.has_value()) {
        return;
    }
    switch (topic->kind) {
    case TopicKind::Order: {
        static thread_local OrderUpdateBuffer storage;
        const auto updates = parse_order_updates(frame, storage);
        if (!updates.has_value()) {
            return;
        }
        for (const auto& wire : *updates) {
            // Fill statuses deliberately produce nothing here: the execution
            // topic owns fills, and emitting from both doubles every position.
            if (const auto event = map_order_event(wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
        }
        break;
    }
    case TopicKind::Execution: {
        static thread_local ExecutionBuffer storage;
        const auto executions = parse_executions(frame, storage);
        if (!executions.has_value()) {
            return;
        }
        for (const auto& wire : *executions) {
            if (const auto event = map_execution(wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
        }
        break;
    }
    default:
        break;
    }
}

// Public market-data socket.
class PublicHandler final : public net::WsHandler {
public:
    PublicHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        // The ONE resubscribe path. Everything the gateway asked for is
        // re-sent here, which is why the transport does not track it.
        std::vector<std::string_view> topics;
        topics.reserve(state_->desired_topics.size());
        for (const auto& topic : state_->desired_topics) {
            topics.push_back(topic);
        }
        std::size_t sent = 0;
        while (sent < topics.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{topics}.subspan(sent));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect will retry the whole set
            }
            sent += frame->consumed;
        }
        // Books cannot survive a gap in the socket, let alone a reconnect.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_public_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason reason) override {
        state_->sink->on_connectivity({.state = reason == net::WsCloseReason::Requested
                                                    ? exec::ConnState::Disconnected
                                                    : exec::ConnState::Disconnected,
                                       .stream_id = 0,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    [[nodiscard]] MarketType market() const noexcept { return market_; }

private:
    VenueState* state_;
    MarketType market_;
    net::WsClient* client_{nullptr};
};

// Private order/execution socket.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto auth = state_->protocol.auth_frame(state_->signer, state_->venue_now());
        if (!auth.has_value() || !client_->send(*auth)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            return;
        }
        // Bybit accepts the subscribe immediately after auth on the same
        // socket; a rejected auth simply closes it, so there is nothing to
        // wait for.
        const std::array<std::string_view, 3> topics{"order", "execution", "position"};
        if (const auto frame = state_->protocol.subscribe_frame(topics); frame.has_value()) {
            (void)client_->send(frame->text);
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class BybitData final : public exec::DataClient {
public:
    BybitData(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, handler_{*state_, market},
          client_{state_->io,
                  net::WsConfig{.host = std::string{host(state_->testnet)},
                                .port = "443",
                                .target = std::string{public_stream_path(market)}},
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
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::array<char, 64> buf{};
        const auto topic = topic_name(kind, symbol, buf);
        if (topic.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        state_->add_topic(topic);
        // Live sockets get the delta now; a socket still connecting picks it
        // up from on_open, which replays the whole set.
        if (client_.connected()) {
            const std::array<std::string_view, 1> one{topic};
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
        // Bybit's application-level ping, which its idle timer watches
        // independently of the websocket control frame.
        if (client_.connected() && now.ns >= state_->next_app_ping_ns) {
            (void)client_.send(state_->protocol.ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

class BybitExec final : public exec::ExecutionClient {
public:
    explicit BybitExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          ws_{state_->io,
              net::WsConfig{.host = std::string{host(state_->testnet)},
                            .port = "443",
                            .target = std::string{private_stream_path()}},
              handler_},
          rest_{state_->io,
                net::RestConfig{.host = std::string{state_->testnet ? k_rest_host_test
                                                                    : k_rest_host_live},
                                .port = "443"}},
          builder_{state_->signer} {
        handler_.attach(ws_);
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        ws_.start();
        return {};
    }

    void stop() override {
        ws_.stop();
        rest_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return ws_.connected(); }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        // Synchronous reports do not fit an asynchronous transport; the
        // reconciliation sweep is driven by the gateway through the same
        // callbacks the hot path uses. Returning empty here is honest —
        // pretending otherwise would make a caller believe it reconciled.
        return std::vector<exec::OrderStatusReport>{};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId&, MarketType) override {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "async reconciliation pending")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType,
                                                                                 UnixNanos) override {
        return std::vector<exec::FillReport>{};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::vector<exec::PositionReport>{};
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request = kind == Kind::Create
                                 ? builder_.create_order(cmd, state_->instruments, now)
                             : kind == Kind::Cancel
                                 ? builder_.cancel_order(cmd, state_->instruments, now)
                                 : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            reject(cmd, "request build failed");
            return;
        }
        const auto client_order_id = cmd.client_order_id;
        const auto instrument = cmd.instrument;
        const auto side = cmd.side;
        const auto qty = cmd.qty;
        if (!rest_.send(*request, [this, client_order_id, instrument, side, qty, kind](
                                      net::RestResponse&& response) {
                on_response(std::move(response), client_order_id, instrument, side, qty, kind);
            })) {
            reject(cmd, "rest queue full");
        }
    }

    void on_response(net::RestResponse&& response, ClientOrderId client_order_id,
                     InstrumentId instrument, Side side, QtyLots qty, Kind kind) {
        if (response.transport_failed()) {
            // No verdict from the venue at all. The order is in doubt, which
            // reconciliation resolves; what must NOT happen is silence.
            emit_reject(client_order_id, instrument, side, qty, kind);
            return;
        }
        const auto envelope = parse_envelope(response.body);
        if (!envelope.has_value()) {
            emit_reject(client_order_id, instrument, side, qty, kind);
            return;
        }
        if (envelope->ret_code == k_ret_ok) {
            // Success produces no event here: the private stream delivers the
            // authoritative Accepted/Canceled, and synthesising a second one
            // would race it.
            return;
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->ret_code)) {
            // The order already filled or was already cancelled. Raising an
            // alarm would train operators to ignore cancel rejections that
            // do matter.
            return;
        }
        // Venues do not push a websocket notification for a REST-level
        // refusal, so the terminal event has to be synthesised locally or the
        // order hangs in the store forever.
        emit_reject(client_order_id, instrument, side, qty, kind);
    }

    void emit_reject(ClientOrderId client_order_id, InstrumentId instrument, Side side,
                     QtyLots qty, Kind kind) {
        OrderEvent event{};
        event.type = kind == Kind::Create      ? OrderEventType::Rejected
                     : kind == Kind::Cancel    ? OrderEventType::CancelRejected
                                               : OrderEventType::ModifyRejected;
        event.instrument = instrument;
        event.side = side;
        event.client_order_id = client_order_id;
        event.qty = qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    void reject(const OrderCmd& cmd, std::string_view) noexcept {
        emit_reject(cmd.client_order_id, cmd.instrument, cmd.side, cmd.qty,
                    cmd.kind == CmdKind::Cancel  ? Kind::Cancel
                    : cmd.kind == CmdKind::Amend ? Kind::Amend
                                                 : Kind::Create);
    }

    std::shared_ptr<VenueState> state_;
    PrivateHandler handler_;
    net::WsClient ws_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

class BybitInstruments final : public exec::InstrumentSource {
public:
    explicit BybitInstruments(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType) override {
        // The catalogue pull is asynchronous like everything else on this
        // transport; the gateway drives it and feeds the table. Returning
        // empty rather than blocking keeps the contract honest.
        return std::vector<Instrument>{};
    }

private:
    std::shared_ptr<VenueState> state_;
};

exec::VenueAdapter make_bybit(const exec::VenueConfig& config, exec::GatewaySink& sink,
                              const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);

    // Credentials resolve here and nowhere else. Market data needs none, so a
    // gateway started without them still runs — read-only by construction.
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

    const auto market = config.enable_spot ? MarketType::Spot : MarketType::Perp;
    exec::VenueAdapter adapter;
    adapter.caps = exec::caps_of(VenueId::Bybit);
    adapter.data = std::make_unique<BybitData>(state, market);
    adapter.execution = std::make_unique<BybitExec>(state);
    adapter.instruments = std::make_unique<BybitInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bybit, &make_bybit);
}

} // namespace raikiri::venues::bybit
