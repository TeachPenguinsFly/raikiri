#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::bitget {

namespace {

// Bitget drops a connection idle for 30 s and does not count websocket
// control frames toward that timer, so the app ping runs inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string inst_id;
};

// State every bitget connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref,
               const Clock& clock_ref,
               bool testnet_flag,
               MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // One process serves one market: the table interns by exchange symbol and
    // BTCUSDT names both a spot pair and a perpetual, so loading both
    // catalogues into one table would drop half of the second one.
    MarketType market;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    net::Signer signer;
    // Bitget authenticates with three pieces, not two. The passphrase lives
    // in the same locked, zeroizing storage as the secret and is only ever
    // viewed, never copied into a header the adapter owns.
    config::Secret passphrase;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    // Bitget refuses a signature more than 30 s from ITS clock, so a laptop
    // that drifts looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    [[nodiscard]] std::string_view product() const noexcept { return product_type(testnet); }

    // True when the pair was new.
    bool add_subscription(std::string_view channel, std::string_view inst_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.inst_id == inst_id) {
                return false; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{inst_id});
        return true;
    }
};

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
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
    case EventKind::Books: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        // Terminal for the channel it names, and silence is how it would otherwise present.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::Orders:
    case EventKind::LoginAck:
        break; // the private socket's, and it never shares this handler
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path, which is why the transport tracks none.
        const auto type = inst_type(state_->market, state_->testnet);
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back(
                {.inst_type = type, .channel = entry.channel, .inst_id = entry.inst_id});
        }
        std::size_t sent = 0;
        while (sent < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(sent));
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

class BitgetData final : public exec::DataClient {
public:
    explicit BitgetData(std::shared_ptr<VenueState> state)
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
        const bool added = state_->add_subscription(channel, symbol);
        if (added && client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.inst_type = inst_type(state_->market, state_->testnet),
                             .channel = channel,
                             .inst_id = symbol}};
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
        if (client_.connected() && now.ns >= state_->next_app_ping_ns) {
            (void)client_.send(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

// Cold-path request/response. Every caller is a startup or reconciliation
// path, never the hot loop, so pumping the reactor until the answer arrives is
// exactly right: a gateway that trades before it knows its tick sizes is worse
// than one that takes an extra second to boot.
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

// The response body of a checked call. An empty result and a refused call
// must never be told apart downstream by an empty vector.
std::expected<std::string, exec::VenueError> fetch_checked(VenueState& state,
                                                           net::RestClient& rest,
                                                           const net::RestRequest& request,
                                                           std::uint64_t timeout_ms) {
    const auto response = await_response(state, rest, request, timeout_ms);
    if (!response.has_value()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response before deadline")};
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
    if (envelope->code != k_ok) {
        return std::unexpected{exec::VenueError::make(
            classify(envelope->code), code_number(envelope->code), envelope->msg)};
    }
    return response->body;
}

void sync_clock(VenueState& state, net::RestClient& rest) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, state.builder.server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return; // no correction is better than a wrong one
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

// Private order stream. Login FIRST and subscribe only once Bitget answers:
// a subscribe sent before the login lands is refused with 30004 and never
// retried, leaving a socket that is up, silent, and indistinguishable from a
// quiet account.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto login = state_->protocol.login_frame(
            state_->signer, state_->passphrase.view(), state_->venue_now());
        if (!login.has_value() || !client_->send(*login)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return; // the bare "pong", and whatever Bitget adds next
        }
        switch (event->kind) {
        case EventKind::LoginAck:
            subscribe_orders();
            state_->sink->on_connectivity({.state = exec::ConnState::Connected,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            break;
        case EventKind::Orders: {
            static thread_local OrderUpdateBuffer storage;
            const auto updates = parse_order_updates(payload, storage);
            if (!updates.has_value()) {
                return;
            }
            for (const auto& wire : *updates) {
                if (const auto mapped = map_order_update(wire, state_->instruments, received);
                    mapped.has_value()) {
                    state_->sink->on_order_event(*mapped);
                }
            }
            break;
        }
        case EventKind::Error: {
            const auto err = parse_stream_error(payload);
            if (!err.has_value()) {
                return;
            }
            state_->last_error_code = err->code;
            if (is_login_failure(err->code)) {
                // Bitget closes the socket when a login fails, so the
                // reconnect will fail identically until a human intervenes.
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = 1,
                                               .ts = state_->clock->wall()});
            } else {
                ++state_->subscribe_refusals;
            }
            break;
        }
        case EventKind::SubscribeAck:
        case EventKind::Ticker:
        case EventKind::Trade:
        case EventKind::Books:
        case EventKind::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void subscribe_orders() noexcept {
        const std::array<Subscription, 1> orders{
            Subscription{.inst_type = inst_type(state_->market, state_->testnet),
                         .channel = k_channel_orders,
                         .inst_id = k_all_instruments}};
        if (const auto frame = state_->protocol.subscribe_frame(orders); frame.has_value()) {
            (void)client_->send(frame->text);
        }
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class BitgetExec final : public exec::ExecutionClient {
public:
    explicit BitgetExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          ws_{state_->io,
              net::WsConfig{.host = std::string{stream_host()},
                            .port = std::string{stream_port()},
                            .target = std::string{private_stream_path()}},
              handler_},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        handler_.attach(ws_);
    }

    std::expected<void, exec::VenueError> start() override {
        // The passphrase view is taken here, not in the constructor: it
        // reflects the secret's length at the moment it is read, and start()
        // is the first point at which every credential is certainly resolved.
        builder_.set_credentials(state_->signer, state_->passphrase.view());
        rest_.start();
        sync_clock(*state_, rest_);
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

    // Bitget has no in-place amend on either API. Mix `modify-order` requires
    // a newClientOid and creates a DIFFERENT order asynchronously; spot only
    // offers `cancel-replace-order`, which does the same. An OrderCmd carries
    // one client id and no second one to give the replacement, so honouring
    // either would leave the OMS tracking an order the venue has cancelled.
    // The caps row says amend_order = false and callers cancel and replace
    // with two ids of their own — which is what Bitget does internally anyway.
    void amend(const OrderCmd& cmd) noexcept override {
        emit(cmd,
             OrderEventType::ModifyRejected,
             exec::VenueError::make(
                 exec::ErrorClass::Terminal, 0, "bitget has no in-place amend"));
    }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        std::size_t sent = 0;
        while (sent < cmds.size()) {
            const auto remaining = cmds.subspan(sent);
            const auto batch = builder_.cancel_batch(
                remaining, state_->instruments, state_->product(), state_->venue_now());
            if (!batch.has_value() || batch->consumed == 0) {
                // One command the batch cannot express must not strand the
                // rest of the panic cancel behind it.
                cancel(remaining.front());
                ++sent;
                continue;
            }
            std::vector<OrderCmd> members{remaining.begin(),
                                          remaining.begin() +
                                              static_cast<std::ptrdiff_t>(batch->consumed)};
            if (!rest_.send(batch->request,
                            [this, members](net::RestResponse&& response) {
                                on_batch_response(std::move(response), members);
                            })) {
                for (const auto& member : members) {
                    emit(member,
                         OrderEventType::CancelRejected,
                         exec::VenueError::make(
                             exec::ErrorClass::Retryable, 0, "rest queue full"));
                }
            }
            sent += batch->consumed;
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        auto storage = std::make_unique<OrderUpdateBuffer>();
        std::string body;
        const auto rows = pull_orders(
            builder_.open_orders(market, state_->product(), state_->venue_now()),
            market,
            *storage,
            body);
        if (!rows.has_value()) {
            return std::unexpected{rows.error()};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        auto storage = std::make_unique<OrderUpdateBuffer>();
        std::string body;
        const auto rows = pull_orders(
            builder_.order_by_client_id(id, market, state_->product(), state_->venue_now()),
            market,
            *storage,
            body);
        if (!rows.has_value()) {
            return std::unexpected{rows.error()};
        }
        for (const auto& wire : *rows) {
            if (wire.client_oid != id.view()) {
                continue;
            }
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                return *report;
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        using Reader =
            std::expected<std::span<const WireFill>, WireError> (*)(std::string_view, FillBuffer&);
        Reader read = nullptr;
        switch (market) {
        case MarketType::Spot:
            read = &parse_spot_fills;
            break;
        case MarketType::Perp:
            read = &parse_mix_fills;
            break;
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("bitget delivery futures fill pull")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        const auto request =
            builder_.fills(market, state_->product(), since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        const auto body = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<FillBuffer>();
        const auto rows = read(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_fill_report(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        switch (market) {
        case MarketType::Perp:
            break;
        case MarketType::Spot:
            // Spot holds coin balances, not per-instrument positions: there is
            // nothing to report against an InstrumentId, and answering "flat"
            // would tell a reconciler the account is empty.
            return std::unexpected{exec::not_implemented("bitget spot position pull")};
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("bitget delivery futures position pull")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        const auto request = builder_.positions(market, state_->product(), state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        const auto body = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<PositionBuffer>();
        const auto rows = parse_positions(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel };

    static constexpr std::uint64_t k_pull_timeout_ms = 10'000;

    // Both order pulls answer with the same rows under different envelopes,
    // so the market chooses the reader and names its own refusal in one place.
    //
    // `body` is the CALLER's: every returned view points into it, so a body
    // owned here would be freed before the first row was read.
    std::expected<std::span<const WireOrderUpdate>, exec::VenueError>
    pull_orders(const std::expected<net::RestRequest, BuildError>& request,
                MarketType market,
                OrderUpdateBuffer& storage,
                std::string& body) {
        using Reader = std::expected<std::span<const WireOrderUpdate>, WireError> (*)(
            std::string_view, OrderUpdateBuffer&);
        Reader read = nullptr;
        switch (market) {
        case MarketType::Spot:
            read = &parse_spot_orders;
            break;
        case MarketType::Perp:
            read = &parse_mix_orders;
            break;
        case MarketType::Future:
            // Delivery contracts never reach the instrument table, so every
            // row would map to nothing and the pull would read as "nothing open".
            return std::unexpected{exec::not_implemented("bitget delivery futures order pull")};
        case MarketType::Option:
            // Bitget v2 lists no crypto options; its only options product is
            // US equity options behind the separate v3 UTA API.
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        auto fetched = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!fetched.has_value()) {
            return std::unexpected{fetched.error()};
        }
        body = std::move(*fetched);
        const auto rows = read(body, storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        return *rows;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create
                ? builder_.create_order(cmd, state_->instruments, state_->product(), now)
                : builder_.cancel_order(cmd, state_->instruments, state_->product(), now);
        if (!request.has_value()) {
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(
                     exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const OrderCmd snapshot = cmd; // 128 trivially-copyable bytes
        if (!rest_.send(*request, [this, snapshot, kind](net::RestResponse&& response) {
                on_response(std::move(response), snapshot, kind);
            })) {
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
        }
    }

    static OrderEventType refusal_of(Kind kind) noexcept {
        // CancelRejected, not Rejected: the order a cancel targets is alive.
        return kind == Kind::Create ? OrderEventType::Rejected : OrderEventType::CancelRejected;
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        if (response.transport_failed()) {
            emit(cmd,
                 refusal_of(kind), // no verdict from the venue at all
                 exec::VenueError::make(
                     exec::ErrorClass::Connectivity, 0, "no response from venue"));
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(
                     exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code == k_ok) {
            // The acceptance itself arrives on the private socket; the REST
            // answer carries only ids, and an id without a lifecycle event is
            // a second source of truth nobody reads.
            return;
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->code)) {
            return;
        }
        emit(cmd,
             refusal_of(kind),
             exec::VenueError::make(
                 classify(envelope->code), code_number(envelope->code), envelope->msg));
    }

    void on_batch_response(net::RestResponse&& response, const std::vector<OrderCmd>& members) {
        const auto reject_all = [&](const exec::VenueError& error) {
            for (const auto& member : members) {
                emit(member, OrderEventType::CancelRejected, error);
            }
        };
        if (response.transport_failed()) {
            reject_all(exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "no response from venue"));
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            reject_all(
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code != k_ok) {
            if (!is_benign_cancel_failure(envelope->code)) {
                reject_all(exec::VenueError::make(
                    classify(envelope->code), code_number(envelope->code), envelope->msg));
            }
            return;
        }
        // A batch answers 00000 while refusing individual orders inside
        // failureList, so a caller that reads only the envelope believes it
        // cancelled orders that are still working.
        auto storage = std::make_unique<BatchFailureBuffer>();
        const auto failures = parse_batch_failures(response.body, *storage);
        if (!failures.has_value()) {
            return;
        }
        for (const auto& failure : *failures) {
            if (is_benign_cancel_failure(failure.code)) {
                continue;
            }
            for (const auto& member : members) {
                if (member.exchange_client_order_id.view() == failure.client_oid) {
                    emit(member,
                         OrderEventType::CancelRejected,
                         exec::VenueError::make(
                             classify(failure.code), code_number(failure.code), failure.msg));
                }
            }
        }
    }

    // The reason is not optional. `OrderEvent` has no room for text, so every
    // refusal hands the sink the verdict that caused it — otherwise an order
    // that dies here is indistinguishable from one that was never sent.
    void emit(const OrderCmd& cmd,
              OrderEventType type,
              const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
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
        // A rejected create leaves nothing working; a refused cancel or modify
        // leaves the order exactly as it was.
        event.leaves_qty = type == OrderEventType::Rejected ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    PrivateHandler handler_;
    net::WsClient ws_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

class BitgetInstruments final : public exec::InstrumentSource {
public:
    explicit BitgetInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_);
        switch (market) {
        case MarketType::Spot:
            return load_spot();
        case MarketType::Perp:
            return load_mix(market);
        case MarketType::Future:
            // Delivery contracts ride the same catalogue as the perps, but the
            // mapper reads symbolType "perpetual" only and would hand back an
            // empty list — which reads as a venue that lists no futures.
            return std::unexpected{exec::not_implemented("bitget delivery futures catalogue")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options catalogue")};
        }
        return std::unexpected{exec::not_implemented("bitget catalogue")};
    }

private:
    static constexpr std::uint64_t k_catalogue_timeout_ms = 20'000;

    template <typename Rows, typename Map>
    std::vector<Instrument> intern(const Rows& rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(rows.size());
        for (const auto& entry : rows) {
            auto instrument = map(entry);
            if (!instrument.has_value()) {
                continue; // not tradable, not this market, or missing a step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_mix(MarketType market) {
        const auto body = fetch_checked(*state_,
                                        rest_,
                                        state_->builder.contracts(state_->product()),
                                        k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire,
                      [market](const WireContract& row) { return map_instrument(row, market); });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        const auto body = fetch_checked(
            *state_, rest_, state_->builder.spot_symbols(), k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<SpotSymbolBuffer>();
        const auto wire = parse_spot_symbols(*body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, [](const WireSpotSymbol& row) { return map_instrument(row); });
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

// Credentials arrive as reference STRINGS, never values. A reference that
// will not resolve leaves the adapter unsigned, and every signed builder then
// refuses rather than sending an unauthenticated order.
void load_credentials(const exec::VenueConfig& config, VenueState& state) {
    config::DefaultSecretProvider provider;
    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state.signer.set_api_key(key.view());
            }
        }
    }
    if (!config.api_secret_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            (void)provider.resolve(*ref, state.signer.secret());
        }
    }
    if (!config.passphrase_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.passphrase_env); ref.has_value()) {
            (void)provider.resolve(*ref, state.passphrase);
        }
    }
}

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    const auto market = config.market;
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, market);
    load_credentials(config, *state);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bitget);
    adapter.data = std::make_unique<BitgetData>(state);
    adapter.execution = std::make_unique<BitgetExec>(state);
    adapter.instruments = std::make_unique<BitgetInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitget, &make);
}

} // namespace raikiri::venues::bitget
