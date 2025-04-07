#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/binance/binance_venue.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::binance {

namespace {

// State every binance connection shares within one gateway process. Single
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
    RequestBuilder builder{signer};

    // Desired subscriptions, owned here rather than by the transport: the
    // socket re-asserts this set on every open, so a reconnect and a cold
    // start run the same path.
    std::vector<std::string> desired_streams;

    // Venue clock offset, measured against /fapi/v1/time. Unused until
    // signing lands, but measured now because a wrong offset shows up as
    // -1021 (INVALID_TIMESTAMP), which reads like a credentials problem.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused. A stream that was never established
    // and a stream that is merely quiet look identical from the sink, which
    // is why the refusal is counted rather than inferred.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    // User-stream key. Valid 60 minutes and EXTENDED by a keepalive rather
    // than reissued — reissuing hands back the same key but resets nothing if
    // the old one already lapsed, so the timer matters more than the call.
    std::string listen_key;
    std::uint64_t listen_key_due_ns{0};
    bool listen_key_expired{false};

    // The gateway pumps one poll(), and it reaches the data client. The
    // execution client's listen-key clock has to advance on the same tick, so
    // it hangs its own here rather than the gateway learning about a second
    // thing to call.
    std::function<void(UnixNanos)> tick_hook;

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_stream(std::string_view stream) {
        for (const auto& existing : desired_streams) {
            if (existing == stream) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_streams.emplace_back(stream);
    }
};

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    const auto data = unwrap(frame);
    if (!data.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::AggTrade: {
        const auto wire = parse_agg_trade(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::MarkPrice: {
        const auto wire = parse_mark_price(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::DepthUpdate:
        // Parsed and tested, but nothing consumes a book yet: BBO comes from
        // bookTicker, so maintaining one here would be state kept for its
        // own sake. The chaining rule it needs is already pinned by tests.
        break;
    case EventKind::Error: {
        // A refused subscription is terminal for the stream it names, and
        // silence is exactly how it would otherwise present. Recorded here so
        // the operator interface can report it; nothing above the adapter can
        // ask a socket what it failed to subscribe to.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::OrderUpdate:
    case EventKind::AccountUpdate:
    case EventKind::ListenKeyExpired:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path: everything the gateway asked for is
        // re-sent here, which is why the transport tracks none of it.
        std::vector<std::string_view> streams;
        streams.reserve(state_->desired_streams.size());
        for (const auto& stream : state_->desired_streams) {
            streams.push_back(stream);
        }
        std::size_t sent = 0;
        while (sent < streams.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{streams}.subspan(sent));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
            sent += frame->consumed;
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

class BinanceData final : public exec::DataClient {
public:
    explicit BinanceData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = "443",
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
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::array<char, 64> buf{};
        const auto stream = stream_name(kind, symbol, buf);
        if (stream.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired_streams.size() >= k_max_streams_per_connection) {
            // One socket takes 1024 streams. Failing here beats discovering
            // it as silence on the 1025th subscription.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "stream limit reached")};
        }
        state_->add_stream(stream);
        if (client_.connected()) {
            const std::array<std::string_view, 1> one{stream};
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
        // No application-level ping: binance drives keepalive with websocket
        // control frames, which the transport already answers. The user
        // stream's listen key does need refreshing, and that rides here.
        if (state_->tick_hook) {
            state_->tick_hook(now);
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

// Handles one user-stream frame. Kept out of the client so dispatch is
// testable without a socket.
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::OrderUpdate: {
        const auto wire = parse_order_update(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto order_event = map_order_event(*wire, state.instruments, received);
            order_event.has_value()) {
            state.sink->on_order_event(*order_event);
        }
        break;
    }
    case EventKind::ListenKeyExpired:
        // The key died before its hour was up, which happens when the account
        // opens a second stream. Nothing here can fix it; the keepalive tick
        // notices the flag and rebuilds the socket.
        state.listen_key_expired = true;
        break;
    case EventKind::AccountUpdate:
        // Positions and balances are pulled on reconciliation rather than
        // tracked incrementally: an incremental position that drifts is worse
        // than one that is a few seconds old.
        break;
    default:
        break;
    }
}

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // Nothing to subscribe to: the listen key in the URL IS the
        // subscription, which is why an expired key presents as silence
        // rather than as an error frame.
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

private:
    VenueState* state_;
};

// Keepalive cadence. The key lapses at 60 minutes; 30 leaves room for one
// missed call before anything is lost.
constexpr std::uint64_t k_listen_key_interval_ns = 30ULL * 60 * 1'000'000'000ULL;

std::optional<net::RestResponse> await_response(VenueState& state,
                                                net::RestClient& rest,
                                                const net::RestRequest& request,
                                                std::uint64_t timeout_ms = 10'000);

class BinanceExec final : public exec::ExecutionClient {
public:
    explicit BinanceExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          handler_{*state_} {
        state_->tick_hook = [this](UnixNanos now) { tick(now); };
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        started_ = true;
        if (!state_->signer.has_credentials()) {
            // Read-only by construction: without keys there is no user stream
            // to open, and every order path already refuses.
            return {};
        }
        return open_user_stream();
    }

    void stop() override {
        if (!started_) {
            return; // teardown calls stop twice
        }
        if (client_) {
            client_->stop();
            client_.reset();
        }
        if (state_->signer.has_credentials() && !state_->listen_key.empty()) {
            if (const auto request = state_->builder.listen_key_close(); request.has_value()) {
                (void)await_response(*state_, rest_, *request, 2'000);
            }
        }
        state_->listen_key.clear();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return client_ != nullptr && client_->connected();
    }

    void submit(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd, OrderEventType::Rejected);
            return;
        }
        const auto request =
            state_->builder.create_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::Rejected);
            return;
        }
        send_order(*request, cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd, OrderEventType::CancelRejected);
            return;
        }
        const auto request =
            state_->builder.cancel_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::CancelRejected);
            return;
        }
        send_order(*request, cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd, OrderEventType::ModifyRejected);
            return;
        }
        const auto request =
            state_->builder.amend_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected);
            return;
        }
        send_order(*request, cmd, OrderEventType::ModifyRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        const auto request = state_->builder.open_orders(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OrderBuffer storage;
        const auto wire = parse_orders(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::OrderStatusReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_open_order(entry, state_->instruments);
                report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType) override {
        // Binance requires a symbol on a single-order query, and the id alone
        // does not carry one. The open-order sweep is what recovers an order
        // whose instrument we no longer know.
        const auto* instrument = first_known_instrument();
        if (instrument == nullptr) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "query needs a loaded catalogue")};
        }
        const auto request = state_->builder.query_order(
            base_of(*instrument).exchange_symbol.view(), id, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_order(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        const auto report = map_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unmappable order")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos since) override {
        const auto* instrument = first_known_instrument();
        if (instrument == nullptr) {
            return std::vector<exec::FillReport>{};
        }
        const auto request = state_->builder.user_trades(
            base_of(*instrument).exchange_symbol.view(), since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local TradeBuffer storage;
        const auto wire = parse_user_trades(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::FillReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_user_trade(entry, state_->instruments);
                report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        const auto request = state_->builder.position_risk(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::array<WirePositionRisk, 512> storage{};
        const auto wire = parse_position_risk(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::PositionReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_position(entry, state_->instruments); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    // Called from the gateway loop by the data client's poll, which owns the
    // io context; this only advances the key's clock.
    void tick(UnixNanos now) noexcept {
        if (!started_ || !state_->signer.has_credentials()) {
            return;
        }
        if (state_->listen_key_expired) {
            state_->listen_key_expired = false;
            (void)open_user_stream();
            return;
        }
        if (now.ns < state_->listen_key_due_ns || state_->listen_key.empty()) {
            return;
        }
        state_->listen_key_due_ns = now.ns + k_listen_key_interval_ns;
        if (const auto request = state_->builder.listen_key_keepalive(); request.has_value()) {
            (void)rest_.send(*request, [](net::RestResponse&&) {});
        }
    }

private:
    [[nodiscard]] const Instrument* first_known_instrument() const noexcept {
        const auto all = state_->instruments.all();
        return all.empty() ? nullptr : &all.front();
    }

    static exec::VenueError build_failed(BuildError err) noexcept {
        return exec::VenueError::make(err == BuildError::NoCredentials
                                          ? exec::ErrorClass::AuthFailure
                                          : exec::ErrorClass::Terminal,
                                      0,
                                      to_string(err));
    }

    // Turns a REST answer into a body, or into the classified failure the
    // gateway acts on. A transport failure is Connectivity, not Terminal: the
    // order may well have reached the venue, and reconciliation settles it.
    [[nodiscard]] std::expected<std::string, exec::VenueError>
    usable_body(const std::optional<net::RestResponse>& response) const {
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body);
            err.has_value() && err->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(err->code), err->code, err->msg)};
        }
        return response->body;
    }

    std::expected<void, exec::VenueError> open_user_stream() {
        const auto request = state_->builder.listen_key_create();
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto key = parse_listen_key(*body);
        if (!key.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "listen key missing")};
        }
        state_->listen_key.assign(*key);
        state_->listen_key_due_ns = state_->clock->mono().ns + k_listen_key_interval_ns;

        if (client_) {
            client_->stop();
        }
        client_ = std::make_unique<net::WsClient>(
            state_->io,
            net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                          .port = "443",
                          .target = "/ws/" + state_->listen_key},
            handler_);
        client_->start();
        return {};
    }

    // A REST order path answers with either the venue's acceptance — which
    // produces no event, because the user stream is authoritative — or a
    // refusal, which must be synthesized locally. Binance does not send a
    // user-stream frame for an order it never accepted, so silence here
    // would leave the order open in the store forever.
    void send_order(const net::RestRequest& request,
                    const OrderCmd& cmd,
                    OrderEventType on_failure) noexcept {
        const auto captured = cmd;
        const auto sent = rest_.send(request, [this, captured, on_failure](net::RestResponse&& r) {
            if (r.transport_failed()) {
                emit(captured, on_failure);
                return;
            }
            const auto err = parse_rest_error(r.body);
            if (!err.has_value() || err->code == k_ok) {
                return; // accepted; the user stream reports what happens next
            }
            if (on_failure == OrderEventType::CancelRejected &&
                is_benign_cancel_failure(err->code)) {
                // The order already filled or was already gone. Reporting a
                // CancelRejected here would alarm an operator about a race
                // that resolved itself.
                return;
            }
            emit(captured, on_failure);
        });
        if (!sent) {
            emit(cmd, on_failure);
        }
    }

    void emit(const OrderCmd& cmd, OrderEventType type) noexcept {
        OrderEvent event{};
        event.type = type;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    PrivateHandler handler_;
    std::unique_ptr<net::WsClient> client_;
    bool started_{false};
};

std::optional<net::RestResponse> await_response(VenueState& state,
                                                net::RestClient& rest,
                                                const net::RestRequest& request,
                                                std::uint64_t timeout_ms) {
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
    // Half the round trip is the usual estimate of one-way delay; the offset
    // only has to be good to a fraction of the recvWindow.
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

class BinanceInstruments final : public exec::InstrumentSource {
public:
    explicit BinanceInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response = await_response(*state_, rest_, state_->builder.instruments(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body);
            err.has_value() && err->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(err->code), err->code, err->msg)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_exchange_info(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, market);
            if (!instrument.has_value()) {
                continue; // not trading, not this market, or missing a filter
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            // The table rewrote the id to the slot that indexes it, so the
            // copy handed back carries the id every inbound frame will use.
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);

    // Credentials resolve here and nowhere else. Market data needs none, so a
    // gateway started without them still runs — read-only by construction
    // rather than by configuration.
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
    adapter.caps = exec::caps_of(VenueId::Binance);
    adapter.data = std::make_unique<BinanceData>(state);
    adapter.execution = std::make_unique<BinanceExec>(state);
    adapter.instruments = std::make_unique<BinanceInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Binance, &make);
}

} // namespace raikiri::venues::binance
