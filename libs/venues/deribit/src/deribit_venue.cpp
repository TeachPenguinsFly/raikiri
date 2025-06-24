#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::deribit {

namespace {

// Deribit's floor is 10 s; near it keeps a quiet option contract's socket
// demonstrably alive rather than merely un-torn-down.
constexpr std::uint32_t k_heartbeat_interval_s = 15;

// Distinguishes the private socket's connectivity from the market-data one in
// everything the gateway logs.
constexpr std::uint64_t k_private_stream_id = 1;

// Owned as a string: the instrument table's views outlive nothing here.
struct OwnedSubscription {
    std::string channel;
};

// State every deribit connection shares within one gateway process. Single
// threaded by contract, which is why nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    Protocol private_protocol; // its own id sequence: two sockets, two streams
    RequestBuilder builder;
    net::Signer signer;

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;

    std::int64_t clock_offset_ns{0};

    std::uint64_t channels_requested{0};
    std::uint64_t channels_accepted{0};
    std::int64_t last_error_code{0};

    // Deribit rejects a signature more than 60 seconds old, so every signed
    // request is stamped with the venue's clock rather than this host's.
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = static_cast<std::int64_t>(clock->wall().ns);
        return UnixNanos{static_cast<std::uint64_t>(local + clock_offset_ns)};
    }

    void add_subscription(std::string_view channel) {
        for (const auto& existing : desired) {
            if (existing.channel == channel) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel});
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

void handle_book(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->instrument);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    const auto result = book.apply(*wire);
    if (result != BookResult::Applied && result != BookResult::Resynced) {
        return; // unsynced, redelivered or dropped: silence, never a guess
    }
    if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

// What an order has left after a frame's trades. The order row beside the
// trade is the authority; failing that, the trade states the ORDER's state
// after itself, and "filled" is the one value that names a quantity.
std::optional<QtyLots> leaves_for(const WireUserTrade& trade,
                                  std::span<const WireOrder> orders,
                                  const exec::InstrumentTable& table) {
    for (const auto& order : orders) {
        if (order.order_id == trade.order_id) {
            return order_leaves(order, table);
        }
    }
    return trade.state == "filled" ? std::optional{QtyLots::from_raw(0)} : std::nullopt;
}

// One user.changes frame: the orders that moved and the trades that moved
// them. The fills go out FIRST so an order that finished in the same frame
// reaches the OMS already accounted for, and each fill's leaves comes from the
// order row beside it rather than from a running total this process kept.
void handle_changes(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local OrderBuffer orders;
    static thread_local UserTradeBuffer trades;
    const auto wire = parse_user_changes(frame, orders, trades);
    if (!wire.has_value()) {
        return;
    }
    for (const auto& trade : wire->trades) {
        const auto leaves = leaves_for(trade, wire->orders, state.instruments);
        if (!leaves.has_value()) {
            // Deliberately silent. The channel's whole design is that an order
            // and the trades that moved it travel together, so this is a shape
            // Deribit does not emit — and a Filled carrying a guessed leaves
            // would either close an order still working or leave a finished
            // one open forever.
            continue;
        }
        if (const auto event = map_fill(trade, state.instruments, *leaves, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
    for (const auto& order : wire->orders) {
        if (const auto event = map_order_event(order, state.instruments, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
}

void handle_frame(VenueState& state,
                  net::WsClient& client,
                  std::string_view frame,
                  UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::Bbo: {
        const auto wire = parse_bbo(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
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
        // Dated futures share this channel and quote no funding at all, and
        // options carry greeks instead. One ticker, three kinds of row.
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto greeks = map_greeks(*wire, state.instruments, received);
            greeks.has_value()) {
            state.sink->on_option_greeks(*greeks);
        }
        break;
    }
    case EventKind::Book:
        handle_book(state, frame, received);
        break;
    case EventKind::UserChange:
        handle_changes(state, frame, received);
        break;
    case EventKind::Heartbeat:
        // An unanswered test_request costs the connection.
        if (event->needs_test_reply) {
            (void)client.send(state.protocol.test_frame().text);
        }
        break;
    case EventKind::Response: {
        const auto response = parse_response(frame);
        if (!response.has_value()) {
            return;
        }
        if (response->failed()) {
            state.last_error_code = response->error_code;
            break;
        }
        state.channels_accepted += response->accepted;
        break;
    }
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // Re-armed per connection: set_heartbeat is a session property.
        (void)client_->send(state_->protocol.set_heartbeat_frame(k_heartbeat_interval_s).text);

        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.channel = entry.channel});
        }
        state_->channels_requested = 0;
        state_->channels_accepted = 0;
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
            state_->channels_requested += frame->consumed;
        }
        // Whatever the change_id chain said before the socket dropped says
        // nothing about the snapshot Deribit is about to send.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, *client_, payload, received);
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

class DeribitData final : public exec::DataClient {
public:
    explicit DeribitData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
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
        std::array<char, k_max_channel_len> buffer{};
        const auto channel = channel_for(kind, symbol, buffer);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        const auto before = state_->desired.size();
        state_->add_subscription(channel);
        if (state_->desired.size() == before) {
            return {};
        }
        if (client_.connected()) {
            const std::array<Subscription, 1> one{Subscription{.channel = channel}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                if (client_.send(frame->text)) {
                    state_->channels_requested += frame->consumed;
                }
            }
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        (void)now; // Deribit drives its own keepalive; nothing is due on a timer
        state_->io.poll();
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
    // Half the round trip is the usual estimate of one-way delay.
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}


// A refusal to build a request is terminal by construction: the same command
// built again produces the same refusal.
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// The private socket. Deribit authenticates the CONNECTION rather than each
// message: public/auth once, and every private call afterwards travels without
// a token. The subscription that matters is only legal once that succeeded,
// which is why it is sent from the auth reply and nowhere else.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        (void)client_->send(
            state_->private_protocol.set_heartbeat_frame(k_heartbeat_interval_s).text);
        const auto frame = state_->private_protocol.auth_frame(state_->signer,
                                                               state_->venue_now());
        if (!frame.has_value() || !client_->send(frame->text)) {
            fail();
            return;
        }
        auth_id_ = frame->id;
        authenticated_ = false;
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return;
        }
        if (event->kind == EventKind::Response) {
            const auto response = parse_response(payload);
            if (response.has_value() && response->id == auth_id_) {
                on_auth(*response);
                return;
            }
        }
        handle_frame(*state_, *client_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        authenticated_ = false;
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }

private:
    void fail() {
        state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_auth(const WireResponse& response) {
        if (response.failed()) {
            state_->last_error_code = response.error_code;
            fail();
            return;
        }
        authenticated_ = true;
        // One channel per (kind, currency): the whole account in four, and a
        // fill on an instrument nothing subscribed to still arrives.
        std::array<std::array<char, k_max_channel_len>, 4> storage{};
        std::array<Subscription, 4> channels{};
        std::size_t count = 0;
        for (const auto market : {MarketType::Perp, MarketType::Option}) {
            for (const auto currency : k_currencies) {
                const auto channel =
                    user_changes_channel(kind_of(market), currency, storage[count]);
                if (channel.empty()) {
                    fail();
                    return;
                }
                channels[count] = Subscription{.channel = channel};
                ++count;
            }
        }
        const auto frame =
            state_->private_protocol.private_subscribe_frame(std::span{channels}.first(count));
        if (!frame.has_value() || !client_->send(frame->text)) {
            fail();
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    std::int64_t auth_id_{0};
    bool authenticated_{false};
};

// Live order entry.
//
// Order entry rides REST while the LIFECYCLE rides the private socket, and the
// division is deliberate: the socket is the single source of every order event,
// so a fill can never be booked twice by arriving on both. The one thing REST
// synthesizes is a rejection, because an order the venue refused produces no
// push at all — there is nothing for the socket to say about an order that
// never existed.
class DeribitExec final : public exec::ExecutionClient {
public:
    explicit DeribitExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_},
          builder_{state_->signer} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signer.has_credentials()) {
            // Nothing below can work, and saying so here sends an operator to
            // the config instead of to a venue status page.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "deribit: no credentials configured")};
        }
        rest_.start();
        sync_clock(*state_, rest_);
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return started_ && client_.connected() && handler_.authenticated();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        std::vector<exec::OrderStatusReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.open_orders(market, currency, state_->venue_now());
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
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
            }
            for (const auto& wire : *orders) {
                auto report = map_open_order(wire, state_->instruments);
                // kind=future answers for the perpetual AND the dated series;
                // the instrument table is what separates the two markets.
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // The ULID went out as the label and comes back as one, so no local
        // table ties the OMS's id to the venue's — but the lookup is scoped by
        // currency and nothing in an id says which coin it was for.
        for (const auto currency : k_currencies) {
            const auto request = builder_.order_state_by_label(id, currency, state_->venue_now());
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
                continue;
            }
            for (const auto& wire : *orders) {
                auto report = map_open_order(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    return *report;
                }
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "deribit: label not found")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        std::vector<exec::FillReport> reports;
        for (const auto currency : k_currencies) {
            const auto request =
                builder_.user_trades(market, currency, since, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failure(request.error())};
            }
            const auto body = fetch(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local UserTradeBuffer storage;
            const auto trades = parse_user_trades(*body, storage);
            if (!trades.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
            }
            for (const auto& wire : *trades) {
                auto report = map_fill_report(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        const auto now = state_->venue_now();
        std::vector<exec::PositionReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.positions(market, currency, now);
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
            for (const auto& wire : *positions) {
                // Deribit stamps no time on a position row, so the read's own
                // clock is the only honest answer to "as of when".
                auto report = map_position(wire, state_->instruments, now);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

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
        // A JSON-RPC error rides a 200, so the envelope decides, not the
        // status line.
        const auto envelope = parse_response(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (envelope->failed()) {
            return std::unexpected{exec::VenueError::make(
                classify(envelope->error_code), envelope->error_code, envelope->error_message)};
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
            emit_reject(cmd, kind);
            return;
        }
        if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            emit_reject(cmd, kind);
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        if (response.transport_failed()) {
            emit_reject(cmd, kind); // no verdict from the venue at all
            return;
        }
        const auto envelope = parse_response(response.body);
        const bool http_ok = response.status >= 200 && response.status < 300;
        if (http_ok && envelope.has_value() && !envelope->failed()) {
            // The socket announces what actually happened; announcing it here
            // as well is how one fill becomes two. That includes a
            // cancel_by_label that matched NOTHING — its result is a count,
            // and a count of zero means the order was already gone, which the
            // socket has therefore already reported as terminal.
            return;
        }
        if (kind != Kind::Create && envelope.has_value() &&
            is_settled_order_refusal(envelope->error_code)) {
            // The order finished between the decision and the request. The
            // venue is agreeing with a state we had not heard about yet, and a
            // CancelRejected here would be raised against an order the FSM has
            // already closed.
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
    PrivateHandler handler_;
    net::RestClient rest_;
    net::WsClient client_;
    SignedRequestBuilder builder_;
    bool started_{false};
};

class DeribitInstruments final : public exec::InstrumentSource {
public:
    explicit DeribitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        std::string_view kind;
        switch (market) {
        case MarketType::Perp:
        case MarketType::Future:
            kind = "future";
            break;
        case MarketType::Option:
            kind = "option";
            break;
        case MarketType::Spot:
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "deribit spot is not modelled")};
        }

        rest_.start();
        sync_clock(*state_, rest_);

        std::vector<Instrument> loaded;
        for (const auto currency : k_currencies) {
            const auto response =
                await_response(*state_, rest_, state_->builder.instruments(currency, kind), 20'000);
            if (!response.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
            }
            if (response->transport_failed()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
            }
            const auto envelope = parse_response(response->body);
            if (!envelope.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
            }
            if (envelope->failed()) {
                return std::unexpected{exec::VenueError::make(
                    classify(envelope->error_code), envelope->error_code, envelope->error_message)};
            }

            static thread_local InstrumentBuffer storage;
            const auto wire = parse_instruments(response->body, storage);
            if (!wire.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            state_->instruments.reserve(state_->instruments.size() + wire->size());
            for (const auto& entry : *wire) {
                auto instrument = map_instrument(entry, market);
                if (!instrument.has_value()) {
                    continue; // inactive, linear, wrong market, or missing a step
                }
                const auto id = state_->instruments.add(*instrument);
                if (!id.has_value()) {
                    continue; // duplicate or table full: reported by count, not by throw
                }
                // The table rewrote the id to the slot that indexes it.
                loaded.push_back(*state_->instruments.find(*id));
            }
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

    // Deribit's client_id is the api key and its client_secret the secret; the
    // signer holds both under the names every other adapter uses.
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
    adapter.caps = exec::caps_of(VenueId::Deribit);
    adapter.data = std::make_unique<DeribitData>(state);
    adapter.execution = std::make_unique<DeribitExec>(state);
    adapter.instruments = std::make_unique<DeribitInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Deribit, &make);
}

} // namespace raikiri::venues::deribit
