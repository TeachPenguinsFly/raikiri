#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::kucoin {

namespace {

// The bullet response states 18 s; this is the fallback if it ever omits it.
constexpr std::uint64_t k_default_ping_ms = 18'000;

// Long enough that a venue outage does not turn into a bullet-public flood,
// short enough that a token rejection costs one second of data.
constexpr std::uint64_t k_reopen_delay_ns = 1'000'000'000ULL;

struct OwnedSubscription {
    std::string topic;
    std::string symbol;
};

// gatewayd pumps DataClient::poll and nothing else, so anything with a timer
// of its own registers here. The private sockets need one: KuCoin closes a
// connection whose application-level ping stops arriving, and the websocket
// control frame does not count as one.
class Pumped {
public:
    Pumped() = default;
    Pumped(const Pumped&) = delete;
    Pumped& operator=(const Pumped&) = delete;

    virtual void pump(UnixNanos now) noexcept = 0;

protected:
    ~Pumped() = default;
};

// State every kucoin connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, so nothing locks.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;

    // The one market this process serves. KuCoin's two trees are separate
    // deployments reached through separate bullets, and they share a subject
    // name ("level2") for two different payloads, so nothing below can read a
    // frame without knowing which socket it arrived on.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;
    Credentials credentials;

    std::vector<OwnedSubscription> desired;
    std::vector<Pumped*> pumps;

    // The seed rides its own pool so a book resynchronising never queues
    // behind the bullet token or the catalogue.
    net::RestClient depth_rest{io,
                               net::RestConfig{.host = std::string{rest_host()}, .port = "443"}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::uint64_t app_ping_interval_ns{k_default_ping_ms * 1'000'000ULL};
    std::uint64_t next_app_ping_ns{0};

    bool reopen_due{false};
    std::uint64_t reopen_at_ns{0};
    std::uint64_t connect_seq{0};

    // Measured against /api/v1/timestamp. KuCoin refuses signatures more than
    // five seconds from ITS clock, so a laptop drifting looks exactly like a
    // bad key until the offset is applied.
    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    void add_subscription(std::string_view topic, std::string_view symbol) {
        for (const auto& existing : desired) {
            if (existing.topic == topic && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{topic}, std::string{symbol});
    }

    void schedule_reopen() noexcept {
        reopen_due = true;
        reopen_at_ns = clock->mono().ns + k_reopen_delay_ns;
    }

    [[nodiscard]] std::string next_connect_id() {
        return "raikiri" + std::to_string(clock->wall().ns + connect_seq++);
    }

    DepthBook& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send the pool
// refuses is left alone, because the next change asks again.
void request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        const auto entry = owner->books.find(id.packed);
        if (entry == owner->books.end()) {
            return;
        }
        auto& target = entry->second;
        if (response.transport_failed() || response.status < 200 || response.status >= 300) {
            target.seed_failed();
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_level2_snapshot(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        if (const auto book = target.snapshot(UnixNanos{wire->ts_ns}, response.received);
            book.has_value()) {
            owner->sink->on_depth(*book);
        }
    };
    if (state.depth_rest.send(state.builder.level2_snapshot(base.exchange_symbol.view()),
                              handled)) {
        book.seed_requested();
    }
}

// A whole book off /spotMarket/level2Depth50. No seed, no chain: KuCoin
// republishes the complete depth there, so the only failure available is a
// frame that does not parse — silence, never a stale book served as current.
void handle_book_snapshot(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_snapshot(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    if (const auto depth = map_book(*wire, state.instruments, received); depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_book_change(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto wire = parse_book_change(frame);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->symbol);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    // Asked before the change is offered, so the frame that triggered the seed
    // is itself buffered behind it.
    if (book.wants_seed()) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::Bbo: {
        const auto wire = parse_bbo(frame, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Trade: {
        const auto wire = parse_trade(frame, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::FundingRate: {
        const auto wire = parse_funding_rate(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, wire->symbol, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Depth:
        if (state.market == MarketType::Spot) {
            handle_book_snapshot(state, frame, received);
        } else {
            handle_book_change(state, frame, received);
        }
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            if (is_expired_token(err->code)) {
                state.schedule_reopen();
            } else {
                ++state.subscribe_refusals;
            }
        }
        break;
    }
    case EventKind::MarkPrice:
    case EventKind::OrderChange: // private topic; never reaches this socket
    case EventKind::Welcome:
    case EventKind::Ack:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.topic = entry.topic, .symbol = entry.symbol});
        }
        std::ranges::sort(
            views, [](const Subscription& a, const Subscription& b) { return a.topic < b.topic; });
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
        // The sequence the old book stopped at says nothing about where this
        // subscription resumes, so every book goes back to wanting a seed.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason reason) override {
        if (reason != net::WsCloseReason::Requested) {
            state_->schedule_reopen();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient* client) noexcept { client_ = client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
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

// Turns a REST answer into a usable body, or into the classified failure the
// gateway acts on.
std::expected<std::string, exec::VenueError>
usable_body(const std::optional<net::RestResponse>& response) {
    if (!response.has_value()) {
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Connectivity, 0, "no response before deadline")};
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
        return std::unexpected{
            exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
    }
    return response->body;
}

class KucoinData final : public exec::DataClient {
public:
    explicit KucoinData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->market)}, .port = "443"}} {}

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        state_->depth_rest.start();
        started_ = true;
        return open_stream();
    }

    void stop() override {
        if (client_) {
            client_->stop();
            client_.reset();
        }
        handler_.attach(nullptr);
        state_->reopen_due = false;
        rest_.stop();
        state_->depth_rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return client_ != nullptr && client_->connected();
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // The bullet this socket connected with belongs to ONE tree, and
            // the two trees share neither topics nor payload shapes.
            return std::unexpected{exec::not_implemented("kucoin: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto topic = topic_for(kind, state_->market);
        if (topic.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        state_->add_subscription(topic, symbol);
        if (client_ != nullptr && client_->connected()) {
            const std::array<Subscription, 1> one{Subscription{.topic = topic, .symbol = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_->send(frame->text);
            }
        }
        if (kind == exec::SubKind::Funding) {
            seed_funding(symbol);
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        if (state_->reopen_due && state_->clock->mono().ns >= state_->reopen_at_ns) {
            state_->reopen_due = false;
            reopen();
        }
        state_->io.poll();
        if (connected() && now.ns >= state_->next_app_ping_ns) {
            if (const auto frame = state_->protocol.ping_frame(); !frame.empty()) {
                (void)client_->send(frame);
            }
            state_->next_app_ping_ns = now.ns + state_->app_ping_interval_ns;
        }
        for (auto* pumped : state_->pumps) {
            pumped->pump(now);
        }
    }

private:
    void reopen() noexcept {
        try {
            (void)open_stream().has_value(); // failures schedule their own retry
        } catch (...) {
            state_->schedule_reopen();
        }
    }

    std::expected<void, exec::VenueError> open_stream() {
        const auto body =
            usable_body(await_response(*state_, rest_, state_->builder.bullet_public(), 10'000));
        if (!body.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{body.error()};
        }
        const auto bullet = parse_bullet(*body);
        if (!bullet.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bullet.error()))};
        }
        const auto endpoint =
            make_endpoint(bullet->endpoint, bullet->token, state_->next_connect_id());
        if (!endpoint.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(endpoint.error()))};
        }
        if (bullet->ping_interval_ms != 0) {
            state_->app_ping_interval_ns = bullet->ping_interval_ms * 1'000'000ULL;
        }
        state_->next_app_ping_ns = 0;

        if (client_) {
            client_->stop();
        }
        client_ = std::make_unique<net::WsClient>(state_->io,
                                                  net::WsConfig{.host = endpoint->host,
                                                                .port = endpoint->port,
                                                                .target = endpoint->target},
                                                  handler_);
        handler_.attach(client_.get());
        client_->start();
        return {};
    }

    void seed_funding(std::string_view symbol) {
        std::string owned{symbol};
        (void)rest_.send(state_->builder.funding_rate_current(symbol),
                         [this, owned = std::move(owned)](net::RestResponse&& response) {
                             const auto body =
                                 usable_body(std::optional<net::RestResponse>{std::move(response)});
                             if (!body.has_value()) {
                                 return; // a contract without funding answers 415000
                             }
                             const auto wire = parse_funding_current(*body);
                             if (!wire.has_value()) {
                                 return;
                             }
                             const auto now = state_->clock->wall();
                             if (const auto funding =
                                     map_funding(*wire, owned, state_->instruments, now);
                                 funding.has_value()) {
                                 state_->sink->on_funding(*funding);
                             }
                         });
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::RestClient rest_;
    std::unique_ptr<net::WsClient> client_;
    bool started_{false};
};

// One market family: one host, one signed REST pool, one private socket. Spot
// and futures share nothing but the signing scheme, so nothing is shared here
// either — not the connection, not the paths, not the size units.
class MarketLink final : public net::WsHandler, public Pumped {
public:
    MarketLink(VenueState& state, MarketType market, std::uint32_t stream_id)
        : state_{&state}, market_{market}, stream_id_{stream_id},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(market)}, .port = "443"}},
          builder_{state.credentials} {
        state_->pumps.push_back(this);
    }

    ~MarketLink() { std::erase(state_->pumps, this); }

    [[nodiscard]] MarketType market() const noexcept { return market_; }

    [[nodiscard]] OrderRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] net::RestClient& rest() noexcept { return rest_; }

    void start() {
        rest_.start();
        started_ = true;
        open_stream();
    }

    void stop() {
        if (client_) {
            client_->stop();
            client_.reset();
        }
        rest_.stop();
        started_ = false;
        reopen_due_ = false;
    }

    [[nodiscard]] bool connected() const noexcept {
        return client_ != nullptr && client_->connected();
    }

    void pump(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        if (reopen_due_ && state_->clock->mono().ns >= reopen_at_ns_) {
            reopen_due_ = false;
            open_stream();
        }
        if (connected() && now.ns >= next_ping_ns_) {
            if (const auto frame = protocol_.ping_frame(); !frame.empty()) {
                (void)client_->send(frame);
            }
            next_ping_ns_ = now.ns + ping_interval_ns_;
        }
    }

    void on_open() override {
        const auto topic = private_order_topic(market_);
        const auto frame = protocol_.private_subscribe_frame(topic);
        if (!frame.has_value() || !client_->send(*frame)) {
            // A socket that carries no order topic reports nothing at all,
            // which is indistinguishable from a quiet account.
            schedule_reopen();
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Connected,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_private_event(payload);
        if (!event.has_value()) {
            return; // unknown shapes are ignored, never fatal
        }
        switch (event->kind) {
        case EventKind::OrderChange: {
            const auto wire = parse_order_change(payload);
            if (!wire.has_value()) {
                return;
            }
            if (const auto out = map_order_change(*wire, state_->instruments, received);
                out.has_value()) {
                state_->sink->on_order_event(*out);
            }
            break;
        }
        case EventKind::Error: {
            if (const auto err = parse_stream_error(payload);
                err.has_value() && is_expired_token(err->code)) {
                schedule_reopen(); // the socket's own backoff would reuse the dead token
            }
            break;
        }
        case EventKind::Bbo:
        case EventKind::Trade:
        case EventKind::Depth:
        case EventKind::MarkPrice:
        case EventKind::FundingRate:
        case EventKind::Welcome:
        case EventKind::Ack:
        case EventKind::Pong:
        case EventKind::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason reason) override {
        if (reason != net::WsCloseReason::Requested) {
            // The token is in the URL and expires, so the reconnect has to
            // start from a fresh bullet rather than from the same target.
            schedule_reopen();
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

private:
    void schedule_reopen() noexcept {
        reopen_due_ = true;
        reopen_at_ns_ = state_->clock->mono().ns + k_reopen_delay_ns;
    }

    void open_stream() noexcept {
        const auto request = builder_.bullet_private(market_, state_->venue_now());
        if (!request.has_value()) {
            report_auth_failure();
            return; // no credentials: retrying cannot produce them
        }
        const auto body = usable_body(await_response(*state_, rest_, *request, 10'000));
        if (!body.has_value()) {
            if (body.error().cls == exec::ErrorClass::AuthFailure) {
                // Retrying a refused key at full speed is how an account earns
                // a lockout, so this one failure does not reschedule.
                report_auth_failure();
                return;
            }
            schedule_reopen();
            return;
        }
        const auto bullet = parse_bullet(*body);
        if (!bullet.has_value()) {
            schedule_reopen();
            return;
        }
        const auto endpoint =
            make_endpoint(bullet->endpoint, bullet->token, state_->next_connect_id());
        if (!endpoint.has_value()) {
            schedule_reopen();
            return;
        }
        if (bullet->ping_interval_ms != 0) {
            ping_interval_ns_ = bullet->ping_interval_ms * 1'000'000ULL;
        }
        next_ping_ns_ = 0;

        if (client_) {
            client_->stop();
        }
        client_ = std::make_unique<net::WsClient>(state_->io,
                                                  net::WsConfig{.host = endpoint->host,
                                                                .port = endpoint->port,
                                                                .target = endpoint->target},
                                                  *this);
        client_->start();
    }

    void report_auth_failure() noexcept {
        state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    MarketType market_;
    std::uint32_t stream_id_;
    net::RestClient rest_;
    OrderRequestBuilder builder_;
    Protocol protocol_; // its own, so a private frame never races a public one
    std::unique_ptr<net::WsClient> client_;
    std::uint64_t ping_interval_ns_{k_default_ping_ms * 1'000'000ULL};
    std::uint64_t next_ping_ns_{0};
    std::uint64_t reopen_at_ns_{0};
    bool reopen_due_{false};
    bool started_{false};
};

// Order entry. Every refusal is named: a market this venue does not serve, a
// pull it does not offer and a request that failed to build are three different
// facts, and the reconciler acts on the difference.
class KucoinExec final : public exec::ExecutionClient {
public:
    KucoinExec(std::shared_ptr<VenueState> state, MarketType market) : state_{std::move(state)} {
        // One link, because one process serves one market. The list stays a
        // list: the private stream is per market and the loop below does not
        // care how many there are.
        links_.push_back(std::make_unique<MarketLink>(*state_, market, /*stream_id=*/1));
    }

    std::expected<void, exec::VenueError> start() override {
        if (links_.empty()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin: no market enabled for order entry")};
        }
        if (!state_->credentials.complete()) {
            // Starting anyway would leave every order to fail one at a time,
            // each one looking like a venue problem.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "kucoin: key, secret and passphrase required")};
        }
        for (auto& link : links_) {
            link->start();
        }
        return {};
    }

    void stop() override {
        for (auto& link : links_) {
            link->stop();
        }
    }

    [[nodiscard]] bool connected() const noexcept override {
        if (links_.empty()) {
            return false;
        }
        // Every enabled market must be live: one silent stream is a market
        // whose fills nobody is hearing.
        for (const auto& link : links_) {
            if (!link->connected()) {
                return false;
            }
        }
        return true;
    }

    // The order path's heartbeat, once per market link. The REST pool drops a
    // connection the venue has already closed only if something sweeps it;
    // nothing did, so an idle gateway wrote its next order into a dead socket
    // and read back an end of stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        for (auto& link : links_) {
            link->rest().keepalive(now);
        }
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        // KuCoin's cancel-all takes no order list: it would also kill orders
        // outside this batch, which is a different instruction from the one
        // given. One request each is the only faithful reading.
        for (const auto& cmd : cmds) {
            cancel(cmd);
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        switch (market) {
        case MarketType::Spot: {
            auto* link = link_for(market);
            if (link == nullptr) {
                return std::unexpected{market_not_enabled(market)};
            }
            return spot_open_orders(*link);
        }
        case MarketType::Perp:
        case MarketType::Future: {
            auto* link = link_for(market);
            if (link == nullptr) {
                return std::unexpected{market_not_enabled(market)};
            }
            return futures_open_orders(*link);
        }
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        return std::unexpected{exec::not_implemented("kucoin options order entry")};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        // Neither host can answer this directly: futures offers no read by
        // clientOid at all, and the spot one demands a symbol this signature
        // does not carry. The active sweep is the only route to the answer.
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
        switch (market) {
        case MarketType::Spot:
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        auto* link = link_for(market);
        if (link == nullptr) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = link->builder().fills(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link->rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local FillBuffer storage;
        const auto wire = parse_fills(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(wire->size());
        for (const auto& row : *wire) {
            if (auto report = map_fill(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        switch (market) {
        case MarketType::Spot:
            // KuCoin does have a spot wallet, GET /api/v1/accounts on the other
            // host. Refused rather than answered from the futures overview:
            // they are two accounts, and money moves between them by transfer.
            return std::unexpected{exec::not_implemented("kucoin spot balance pull")};
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        auto* link = link_for(market);
        if (link == nullptr) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = link->builder().account_overview(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link->rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_account_overview(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        const auto balance = map_account_overview(*wire, state_->venue_now());
        if (!balance.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin: account overview names no currency")};
        }
        // ONE ROW: the overview covers one settle currency and this gateway
        // trades one. A second settle currency is a second wallet and a second
        // process, exactly as a second market is.
        return std::vector<BalanceReport>{*balance};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        switch (market) {
        case MarketType::Spot:
            // Spot holds balances, not positions. An empty list would be read
            // as "flat", which is a claim about a concept this market lacks.
            return std::unexpected{exec::not_implemented("kucoin spot position pull")};
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        auto* link = link_for(market);
        if (link == nullptr) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = link->builder().positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link->rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local PositionBuffer storage;
        const auto wire = parse_positions(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(wire->size());
        for (const auto& row : *wire) {
            if (auto report = map_position(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    [[nodiscard]] static exec::VenueError build_failed(BuildError err) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
    }

    [[nodiscard]] static exec::VenueError market_not_enabled(MarketType market) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal,
                                      0,
                                      market == MarketType::Spot
                                          ? "kucoin spot is not enabled on this gateway"
                                          : "kucoin futures is not enabled on this gateway");
    }

    [[nodiscard]] MarketLink* link_for(MarketType market) noexcept {
        for (auto& link : links_) {
            const auto owned = link->market();
            const bool futures = market == MarketType::Perp || market == MarketType::Future;
            if (owned == market || (futures && owned == MarketType::Perp)) {
                return link.get();
            }
        }
        return nullptr;
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    futures_open_orders(MarketLink& link) {
        const auto request = link.builder().open_orders(link.market(), {}, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link.rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OrderBuffer storage;
        const auto wire = parse_futures_orders(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return collect(*wire);
    }

    // KuCoin answers spot actives one symbol at a time, so the sweep asks which
    // symbols have any before asking for them. Two round trips, but the
    // alternative is a per-instrument loop over the whole catalogue.
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    spot_open_orders(MarketLink& link) {
        const auto listing = link.builder().active_symbols(MarketType::Spot, state_->venue_now());
        if (!listing.has_value()) {
            return std::unexpected{build_failed(listing.error())};
        }
        const auto symbols_body = usable_body(await_response(*state_, link.rest(), *listing));
        if (!symbols_body.has_value()) {
            return std::unexpected{symbols_body.error()};
        }
        static thread_local SymbolBuffer symbol_storage;
        const auto symbols = parse_active_symbols(*symbols_body, symbol_storage);
        if (!symbols.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(symbols.error()))};
        }

        std::vector<exec::OrderStatusReport> reports;
        for (const auto symbol : *symbols) {
            const auto request =
                link.builder().open_orders(MarketType::Spot, symbol, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failed(request.error())};
            }
            const auto body = usable_body(await_response(*state_, link.rest(), *request));
            if (!body.has_value()) {
                // A partial sweep read as a complete one is a book that looks
                // emptier than it is, so the whole answer fails together.
                return std::unexpected{body.error()};
            }
            static thread_local OrderBuffer storage;
            const auto wire = parse_spot_orders(*body, storage);
            if (!wire.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            for (const auto& row : *wire) {
                if (auto report = map_open_order(row, state_->instruments); report.has_value()) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::vector<exec::OrderStatusReport> collect(std::span<const WireOrder> rows) const {
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(rows.size());
        for (const auto& row : rows) {
            if (auto report = map_open_order(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        auto* link = link_for(cmd.instrument.market());
        if (link == nullptr) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(
                            exec::ErrorClass::Terminal, 0, "no link for this instrument's market"));
            return;
        }
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? link->builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? link->builder().cancel_order(cmd, state_->instruments, now)
                                   : link->builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const auto client_order_id = cmd.client_order_id;
        const auto exchange_client_order_id = cmd.exchange_client_order_id;
        const auto instrument = cmd.instrument;
        const auto side = cmd.side;
        const auto qty = cmd.qty;
        if (!link->rest().send(
                *request,
                [this, client_order_id, exchange_client_order_id, instrument, side, qty, kind](
                    net::RestResponse&& response) {
                    on_response(std::move(response),
                                client_order_id,
                                exchange_client_order_id,
                                instrument,
                                side,
                                qty,
                                kind);
                })) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
        }
    }

    void on_response(net::RestResponse&& response,
                     ClientOrderId client_order_id,
                     ExchangeClientOrderId exchange_client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind) {
        const auto refuse = [&](const exec::VenueError& error) {
            emit(client_order_id, exchange_client_order_id, instrument, side, qty, kind, error);
        };
        if (response.transport_failed()) {
            refuse(exec::VenueError::make(
                exec::ErrorClass::Connectivity,
                0,
                "no response from venue")); // no verdict from the venue at all
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            refuse(exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code == k_ok) {
            return; // the private socket carries the lifecycle from here
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->code, envelope->msg)) {
            return; // the order was already gone, which is what was asked for
        }
        refuse(exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg));
    }

    void emit_reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        emit(cmd.client_order_id,
             cmd.exchange_client_order_id,
             cmd.instrument,
             cmd.side,
             cmd.qty,
             kind,
             error);
    }

    // A REST refusal never reaches the private socket, so the lifecycle event
    // the OMS is waiting for has to be made here or the order sits open forever.
    //
    // The reason is not optional, and it is demanded HERE rather than in
    // `emit_reject` above: this is the funnel both refusal paths pass through,
    // and the other one holds no OrderCmd to name itself with.
    void emit(ClientOrderId client_order_id,
              ExchangeClientOrderId exchange_client_order_id,
              InstrumentId instrument,
              Side side,
              QtyLots qty,
              Kind kind,
              const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.instrument = instrument;
        event.side = side;
        event.client_order_id = client_order_id;
        event.exchange_client_order_id = exchange_client_order_id;
        event.qty = qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    std::vector<std::unique_ptr<MarketLink>> links_;
};

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

class KucoinInstruments final : public exec::InstrumentSource {
public:
    explicit KucoinInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{
              state_->io,
              net::RestConfig{.host = std::string{rest_host(MarketType::Spot)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        switch (market) {
        case MarketType::Spot:
            return fetch_spot();
        case MarketType::Perp:
            return fetch_futures();
        case MarketType::Future:
            // The futures host lists dated contracts, but every one seen is
            // inverse and the mapper refuses those; admitting them is a
            // separate decision from wiring order entry.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin dated futures are not modelled")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        return std::unexpected{exec::not_implemented("kucoin options order entry")};
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> fetch_futures() {
        rest_.start();
        // Signed requests are refused outside five seconds of KuCoin's clock,
        // so the offset is measured before order entry can need it.
        sync_clock(*state_, rest_);

        const auto body =
            usable_body(await_response(*state_, rest_, state_->builder.contracts_active(), 20'000));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, MarketType::Perp);
            if (!instrument.has_value()) {
                continue; // not open, not a perpetual, inverse, or missing a step
            }
            admit(*instrument, loaded);
        }
        return loaded;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_spot() {
        spot_rest_.start();
        const auto body = usable_body(
            await_response(*state_, spot_rest_, state_->builder.spot_symbols(), 20'000));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        auto storage = std::make_unique<SpotSymbolBuffer>();
        const auto wire = parse_spot_symbols(*body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_spot_symbol(entry);
            if (!instrument.has_value()) {
                continue; // suspended, or missing a usable increment
            }
            admit(*instrument, loaded);
        }
        return loaded;
    }

    void admit(const Instrument& instrument, std::vector<Instrument>& loaded) {
        const auto id = state_->instruments.add(instrument);
        if (!id.has_value()) {
            return; // duplicate or table full: reported by count, not by throw
        }
        loaded.push_back(*state_->instruments.find(*id));
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
};

// Credentials arrive as references to WHERE the secret lives; the value never
// enters the config. The passphrase is sealed last on purpose: key version 2
// signs it with the api secret, so sealing before the secret is loaded would
// produce a digest keyed by nothing.
void load_credentials(const exec::VenueConfig& config, VenueState& state) {
    config::DefaultSecretProvider provider;
    const auto resolve = [&provider](std::string_view spec, config::Secret& into) {
        if (spec.empty()) {
            return false;
        }
        const auto ref = config::SecretRef::parse(spec);
        return ref.has_value() && provider.resolve(*ref, into).has_value();
    };

    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state.credentials.signer().set_api_key(key.view());
            }
        }
    }
    if (!resolve(config.api_secret_env, state.credentials.signer().secret())) {
        return;
    }
    config::Secret passphrase;
    if (resolve(config.passphrase_env, passphrase)) {
        (void)state.credentials.seal_passphrase(passphrase.view());
    }
}

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    load_credentials(config, *state);

    state->market = config.market;

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Kucoin);
    adapter.data = std::make_unique<KucoinData>(state);
    adapter.execution = std::make_unique<KucoinExec>(state, config.market);
    adapter.instruments = std::make_unique<KucoinInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Kucoin, &make);
}

} // namespace raikiri::venues::kucoin
