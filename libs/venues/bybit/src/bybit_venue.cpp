#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bybit/book_sync.hpp>
#include <raikiri/venues/bybit/bybit_venue.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <array>
#include <memory>
#include <optional>
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

    std::vector<std::string> desired_topics;

    std::unordered_map<InstrumentId, BookSync> books;
    std::uint64_t next_app_ping_ns{0};

    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
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
            return;
        }
        const auto ts_event = ms_to_unix(book->ts_ms, received);
        if (const auto bbo = sync.bbo(ts_event, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto depth = sync.snapshot(ts_event, received); depth.has_value()) {
            state.sink->on_depth(*depth);
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

// Cold-path request/response. Every caller of this is a startup or
// reconciliation path, never the hot loop, so pumping the reactor until the
// answer arrives is exactly right: the alternative — an asynchronous
// catalogue load — would force every venue to invent its own half-started
// state, and a gateway that trades before it knows its tick sizes is worse
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

// Defined below, next to the catalogue loader it shares a transport with.
void sync_clock(VenueState& state, net::RestClient& rest);

class PublicHandler final : public net::WsHandler {
public:
    PublicHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
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
                net::RestConfig{
                    .host = std::string{state_->testnet ? k_rest_host_test : k_rest_host_live},
                    .port = "443"}},
          builder_{state_->signer} {
        handler_.attach(ws_);
    }

    std::expected<void, exec::VenueError> start() override {
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

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        const auto request = builder_.open_orders(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local OrderUpdateBuffer storage;
        const auto orders = parse_open_orders(envelope->result, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
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
    fill_reports(MarketType market, UnixNanos since) override {
        const auto request = builder_.executions(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local ExecutionBuffer storage;
        const auto executions = parse_executions(envelope->result, storage);
        if (!executions.has_value()) {
            return std::vector<exec::FillReport>{};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(executions->size());
        for (const auto& wire : *executions) {
            const auto event = map_execution(wire, state_->instruments, state_->clock->wall());
            if (!event.has_value()) {
                continue;
            }
            exec::FillReport report{};
            report.exec_id = event->exec_id;
            report.exchange_order_id = event->exchange_order_id;
            report.exchange_client_order_id = event->exchange_client_order_id;
            report.instrument = event->instrument;
            report.side = event->side;
            report.price = event->price;
            report.qty = event->qty;
            report.fee = event->fee;
            report.liquidity = event->liquidity;
            report.ts_venue = event->ts_event;
            reports.push_back(report);
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local PositionBuffer storage;
        const auto positions = parse_positions(envelope->result, storage);
        if (!positions.has_value()) {
            return std::vector<exec::PositionReport>{};
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

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        // ONE WALLET behind both markets: a unified account funds a spot fill
        // and a perp's margin out of the same coins, so the answer does not
        // vary by market and narrowing it to one would be an invention.
        (void)market;
        const auto request = builder_.wallet_balance(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local CoinBalanceBuffer storage;
        const auto coins = parse_wallet_balance(envelope->result, storage);
        if (!coins.has_value()) {
            // An unreadable wallet is refused rather than reported as an empty
            // vector, which reads as an account holding nothing.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(coins.error()))};
        }
        const auto ts = ms_to_unix(envelope->time_ms, state_->clock->wall());
        std::vector<BalanceReport> reports;
        reports.reserve(coins->size());
        for (const auto& wire : *coins) {
            if (auto report = map_coin_balance(wire, ts); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // The envelope's fields are VIEWS INTO `body`, so the caller owns the
    // body and it outlives every use of them. Returning the two together would
    // leave the views pointing into a string that a move could relocate, and
    // returning the envelope alone — which this did — left them pointing into
    // a response destroyed on the way out. ASan caught it on the first
    // credentialed position pull; nothing without a key ever reaches here.
    [[nodiscard]] std::expected<WireRestEnvelope, exec::VenueError>
    fetch(const net::RestRequest& request, std::string& body) {
        auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        body = std::move(response->body);
        const auto envelope = parse_envelope(body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope")};
        }
        if (envelope->ret_code != k_ret_ok) {
            return std::unexpected{exec::VenueError::make(
                classify(envelope->ret_code), envelope->ret_code, envelope->ret_msg)};
        }
        return *envelope;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            reject(cmd, to_string(request.error()));
            return;
        }
        const auto client_order_id = cmd.client_order_id;
        const auto instrument = cmd.instrument;
        const auto side = cmd.side;
        const auto qty = cmd.qty;
        if (!rest_.send(
                *request,
                [this, client_order_id, instrument, side, qty, kind](net::RestResponse&& response) {
                    on_response(std::move(response), client_order_id, instrument, side, qty, kind);
                })) {
            reject(cmd, "rest queue full");
        }
    }

    void on_response(net::RestResponse&& response,
                     ClientOrderId client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind) {
        if (response.transport_failed()) {
            // No verdict from the venue at all.
            emit_reject(client_order_id,
                        instrument,
                        side,
                        qty,
                        kind,
                        exec::VenueError::make(
                            exec::ErrorClass::Connectivity, 0, "no response from venue"));
            return;
        }
        const auto envelope = parse_envelope(response.body);
        if (!envelope.has_value()) {
            emit_reject(
                client_order_id,
                instrument,
                side,
                qty,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->ret_code == k_ret_ok) {
            return;
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->ret_code)) {
            return;
        }
        emit_reject(client_order_id,
                    instrument,
                    side,
                    qty,
                    kind,
                    exec::VenueError::make(
                        classify(envelope->ret_code), envelope->ret_code, envelope->ret_msg));
    }

    // The reason is not optional. Every refusal here reaches the sink with the
    // verdict that caused it, because the terminal event itself has no room for
    // one and a rejection nobody can explain costs an hour at the exchange.
    void emit_reject(ClientOrderId client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind,
                     const exec::VenueError& error) {
        state_->sink->on_order_error(client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
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

    void reject(const OrderCmd& cmd, std::string_view why) noexcept {
        emit_reject(cmd.client_order_id,
                    cmd.instrument,
                    cmd.side,
                    cmd.qty,
                    cmd.kind == CmdKind::Cancel  ? Kind::Cancel
                    : cmd.kind == CmdKind::Amend ? Kind::Amend
                                                 : Kind::Create,
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, why));
    }

    std::shared_ptr<VenueState> state_;
    PrivateHandler handler_;
    net::WsClient ws_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

class BybitInstruments final : public exec::InstrumentSource {
public:
    explicit BybitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{
                    .host = std::string{state_->testnet ? k_rest_host_test : k_rest_host_live},
                    .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const auto request = builder_.instruments(market);
        const auto response = await_response(*state_, rest_, request, 15'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_envelope(response->body);
        if (!envelope.has_value() || envelope->ret_code != k_ret_ok) {
            return std::unexpected{exec::VenueError::make(
                envelope.has_value() ? classify(envelope->ret_code) : exec::ErrorClass::Terminal,
                envelope.has_value() ? envelope->ret_code : 0,
                "catalogue refused")};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_instruments(envelope->result, storage);
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
                continue; // delisted, pre-launch, or missing a usable filter
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    RequestBuilder builder_{state_->signer};
};

// Bybit refuses signatures outside recvWindow of ITS clock. A laptop drifting
// two seconds therefore looks exactly like bad credentials, so the offset is
// measured once at startup and applied to every signed request. Half the
// round trip is subtracted because the server's timestamp was taken somewhere
// in the middle of it.
void sync_clock(VenueState& state, net::RestClient& rest) {
    net::RestRequest request{};
    request.method = net::HttpMethod::Get;
    request.target = "/v5/market/time";

    const auto before = state.clock->wall();
    const auto response = await_response(state, rest, request, 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return; // no correction is better than a wrong one
    }
    const auto after = state.clock->wall();
    const auto envelope = parse_envelope(response->body);
    if (!envelope.has_value() || envelope->ret_code != k_ret_ok || envelope->time_ms == 0) {
        return;
    }
    const auto server_ns = static_cast<std::int64_t>(envelope->time_ms) * 1'000'000;
    const auto midpoint_ns =
        static_cast<std::int64_t>(before.ns) +
        (static_cast<std::int64_t>(after.ns) - static_cast<std::int64_t>(before.ns)) / 2;
    state.clock_offset_ns = server_ns - midpoint_ns;
}

exec::VenueAdapter
make_bybit(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);

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

    const auto market = config.market;
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
