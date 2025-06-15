#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/gate_venue.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::gate {

namespace {

// Gate closes a connection idle for 60 s of application traffic; the app ping
// runs well inside that window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// Cold-start only, and sized against a measurement — see GateInstruments.
constexpr std::uint32_t k_catalogue_timeout_ms = 20'000;

// Live orders tracked for their leaves. Far above any sane working set; the
// cap exists so a venue incident cannot turn the map into the heap.
constexpr std::size_t k_max_live_orders = 8'192;

constexpr std::uint32_t k_private_stream_id = 1;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string contract;
};

// State every gate connection shares within one gateway process. Single
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
    RequestBuilder builder;

    // The futures private socket demands the account's user id as the first
    // element of every subscribe payload, and answers a payload without it
    // with silence rather than an error. Spot wants no such thing.
    std::uint64_t futures_user_id{0};

    // As much of a live order as Gate states in any one frame. The orders
    // channel carries `left` and our client id; the user-trade channel carries
    // the fill and neither, so a fill cannot state its own leaves — which is
    // the number the OMS treats as authoritative. Keyed by the venue order id,
    // the one field both frames do carry.
    struct OrderMemo {
        ExchangeClientOrderId client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };
    std::unordered_map<std::string, OrderMemo> live_orders;

    // The seed rides its own pool so a book resynchronising never queues
    // behind the catalogue, which is over a megabyte.
    net::RestClient depth_rest{io, net::RestConfig{.host = std::string{rest_host(testnet)}}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    // Venue clock offset, measured against /api/v4/spot/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    void add_subscription(std::string_view channel, std::string_view contract) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.contract == contract) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        auto at = desired.end();
        for (auto it = desired.begin(); it != desired.end(); ++it) {
            if (it->channel == channel) {
                at = it + 1;
            }
        }
        desired.emplace(at, std::string{channel}, std::string{contract});
    }

    [[nodiscard]] std::uint64_t now_s() const noexcept {
        return venue_now().ns / 1'000'000'000ULL;
    }

    // Gate refuses a signature whose Timestamp is more than 60 s from ITS
    // clock, so a laptop drifting a minute looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(const WireOrder& wire) {
        if (wire.order_id.empty()) {
            return;
        }
        if (wire.state == WireOrderState::Cancelled || wire.state == WireOrderState::Expired ||
            wire.state == WireOrderState::Filled) {
            live_orders.erase(std::string{wire.order_id});
            return;
        }
        if (live_orders.size() >= k_max_live_orders &&
            !live_orders.contains(std::string{wire.order_id})) {
            return; // a runaway order count must not become a runaway heap
        }
        auto& memo = live_orders[std::string{wire.order_id}];
        if (const auto leaves = order_leaves(wire, instruments); leaves.has_value()) {
            memo.leaves = *leaves;
        }
        if (const auto body = client_id_from_text(wire.text); !body.empty()) {
            if (const auto id = ExchangeClientOrderId::from(body); id.has_value()) {
                memo.client_order_id = *id;
            }
        }
    }

    [[nodiscard]] OrderMemo memo_for(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? OrderMemo{} : it->second;
    }
};

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send the pool
// refuses is left alone, because the next delta asks again.
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
        const auto wire = parse_order_book(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        if (const auto book = target.snapshot(s_to_unix(wire->update_s), response.received);
            book.has_value()) {
            owner->sink->on_depth(*book);
        }
    };
    if (state.depth_rest.send(
            state.builder.order_book(k_settle_usdt, base.exchange_symbol.view(), k_depth_limit),
            handled)) {
        book.seed_requested();
    }
}

void handle_book_update(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_update(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->contract);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    const auto entry =
        state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    // Asked before the delta is offered, so the frame that triggered the seed
    // is itself buffered behind it.
    if (book.wants_seed()) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(frame);
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
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::BookUpdate:
        handle_book_update(state, frame, received);
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::OrderUpdate:
    case EventKind::UserTrade:
    case EventKind::Pong:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break; // private kinds and control frames are not this socket's
    }
}

// Routes one private frame. The market decides which of Gate's two order
// shapes the payload is; nothing in the frame itself says.
void handle_private_frame(VenueState& state,
                          MarketType market,
                          std::string_view frame,
                          UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::OrderUpdate: {
        static thread_local OrderBuffer orders;
        const auto wire = parse_ws_orders(frame, orders, market);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            // Remembered BEFORE the event is emitted: a user trade for the
            // same fill may already be queued behind this frame, and it can
            // only state its leaves out of this memo.
            state.remember(entry);
            if (const auto mapped = map_order_event(entry, state.instruments, received);
                mapped.has_value()) {
                state.sink->on_order_event(*mapped);
            }
        }
        break;
    }
    case EventKind::UserTrade: {
        static thread_local UserTradeBuffer trades;
        const auto wire = parse_ws_user_trades(frame, trades, market);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            const auto memo = state.memo_for(entry.order_id);
            if (const auto mapped = map_user_trade(entry,
                                                   state.instruments,
                                                   memo.leaves,
                                                   memo.client_order_id,
                                                   received);
                mapped.has_value()) {
                state.sink->on_order_event(*mapped);
            }
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            // Code 4 wraps a REST-style label in its message; the number is
            // enough to tell a bad key from an unknown contract.
            state.sink->on_connectivity(
                {.state = classify_stream(err->code) == exec::ErrorClass::AuthFailure
                              ? exec::ConnState::AuthFailed
                              : exec::ConnState::Connected,
                 .stream_id = k_private_stream_id,
                 .ts = state.clock->wall()});
        }
        break;
    }
    case EventKind::BookTicker:
    case EventKind::Trade:
    case EventKind::Ticker:
    case EventKind::BookUpdate:
    case EventKind::Pong:
    case EventKind::SubscribeAck:
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
            views.push_back({.channel = entry.channel, .contract = entry.contract});
        }
        const auto now_s = state_->now_s();
        std::size_t sent = 0;
        while (sent < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(sent), now_s);
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

class GateData final : public exec::DataClient {
public:
    explicit GateData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        state_->depth_rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != MarketType::Perp) {
            // This socket is fx-ws.gateio.ws /v4/ws/usdt — the futures stream.
            // Gate's spot host appears in this adapter only for the PRIVATE
            // stream; there is no public spot path here. Gate also spells a
            // spot pair and its perpetual the same way, so the futures socket
            // would accept the symbol and return perpetual prints under a spot
            // id, which is worse than no data.
            return std::unexpected{exec::not_implemented("gate spot market data: this socket is the futures stream")};
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
        const auto before = state_->desired.size();
        state_->add_subscription(channel, symbol);
        if (state_->desired.size() == before) {
            return {};
        }
        if (client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.channel = channel, .contract = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one, state_->now_s());
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
        if (client_.connected() && now.ns >= state_->next_app_ping_ns) {
            (void)client_.send(state_->protocol.ping_frame(now.ns / 1'000'000'000ULL));
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
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

// Gate's code is a string label and VenueError carries an integer, so the
// label travels in the message where an operator can still read it.
exec::VenueError rest_failure(std::string_view body) {
    const auto err = parse_rest_error(body);
    if (!err.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    return exec::VenueError::make(classify(err->label), 0, err->label);
}

// Every market this adapter can act on, named exhaustively so Option is a
// case the compiler forces a decision about rather than something a default
// arm swallows. An empty answer here would read as "the venue holds nothing".
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("gate delivery (dated futures) order entry");
    case MarketType::Option:
        // Gate lists options under /api/v4/options, keyed by an underlying and
        // an expiry rather than by a settle currency. Nothing maps an
        // OptionInstrument onto that surface yet.
        return exec::not_implemented("gate options order entry");
    }
    return exec::not_implemented("gate: unknown market");
}

exec::VenueError build_failure(BuildError err) noexcept {
    // A refusal to build is terminal by construction: the same command built
    // again produces the same refusal.
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// One private socket. Futures and spot are different hosts carrying different
// order shapes, so an account trading both holds two of these.
class PrivateHandler final : public net::WsHandler {
public:
    PrivateHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        if (!subscribe(orders_channel(market_)) || !subscribe(user_trades_channel(market_))) {
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
        handle_private_frame(*state_, market_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    [[nodiscard]] bool subscribe(std::string_view channel) {
        if (channel.empty() || client_ == nullptr) {
            return false;
        }
        // The user id is positional and futures-only: sending the contract
        // where Gate expects the id subscribes to nothing and reports success.
        std::array<char, 24> id_text{};
        std::array<std::string_view, 2> payload{};
        std::size_t count = 0;
        if (market_ == MarketType::Perp) {
            const auto [stop, ec] = std::to_chars(
                id_text.data(), id_text.data() + id_text.size(), state_->futures_user_id);
            if (ec != std::errc{} || state_->futures_user_id == 0) {
                return false;
            }
            payload[count++] = {id_text.data(), static_cast<std::size_t>(stop - id_text.data())};
        }
        payload[count++] = k_all_symbols;

        const auto frame = state_->protocol.private_subscribe_frame(
            channel, std::span{payload}.first(count), state_->signer, state_->now_s());
        return frame.has_value() && client_->send(*frame);
    }

    VenueState* state_;
    MarketType market_;
    net::WsClient* client_{nullptr};
};

struct PrivateStream {
    PrivateStream(VenueState& state, MarketType market)
        : handler{state, market},
          client{state.io,
                 net::WsConfig{.host = std::string{private_host(state.testnet, market)},
                               .port = std::string{stream_port()},
                               .target = std::string{private_path(state.testnet, market)}},
                 handler} {
        handler.attach(client);
    }

    PrivateHandler handler;
    net::WsClient client;
};

class GateExec final : public exec::ExecutionClient {
public:
    // WARNING: Gate names both markets' BTC_USDT identically, and the mappers
    // below do NOT name a market — map_bbo, map_trade, map_order_event and
    // map_user_trade all take (wire, table, ts), and WireOrder carries no
    // market field. So a process with BOTH catalogues loaded would have every
    // one of them answer nullptr and drop every frame, silently. Run one
    // gateway per market; the request builders are already market-correct.
    GateExec(std::shared_ptr<VenueState> state, bool spot, bool perp)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          builder_{state_->signer} {
        if (perp) {
            perp_ = std::make_unique<PrivateStream>(*state_, MarketType::Perp);
        }
        if (spot) {
            spot_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot);
        }
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        sync_clock(*state_, rest_);
        if (perp_ && state_->futures_user_id == 0) {
            // Fetched before the socket opens, because on_open cannot ask for
            // it: a subscribe with no user id is answered with an empty stream
            // rather than an error, and that reads as a quiet account.
            if (const auto request = builder_.futures_account(state_->venue_now());
                request.has_value()) {
                if (const auto response = await_response(*state_, rest_, *request);
                    response.has_value() && !response->transport_failed() &&
                    response->status >= 200 && response->status < 300) {
                    if (const auto user = parse_account_user_id(response->body); user.has_value()) {
                        state_->futures_user_id = *user;
                    }
                }
            }
            if (state_->futures_user_id == 0) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::AuthFailure, 0, "gate: futures account id unavailable")};
            }
        }
        if (perp_) {
            perp_->client.start();
        }
        if (spot_) {
            spot_->client.start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (perp_) {
            perp_->client.stop();
        }
        if (spot_) {
            spot_->client.stop();
        }
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        const bool perp_up = perp_ == nullptr || perp_->client.connected();
        const bool spot_up = spot_ == nullptr || spot_->client.connected();
        return started_ && perp_up && spot_up && (perp_ != nullptr || spot_ != nullptr);
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        // Gate caps a batch at 20 and refuses the whole call past it, so the
        // span is chunked rather than truncated.
        for (std::size_t at = 0; at < cmds.size();
             at += SignedRequestBuilder::k_max_batch_cancel) {
            const auto chunk = cmds.subspan(
                at, std::min(SignedRequestBuilder::k_max_batch_cancel, cmds.size() - at));
            const auto request =
                builder_.cancel_batch(chunk, state_->instruments, state_->venue_now());
            if (!request.has_value()) {
                for (const auto& cmd : chunk) {
                    emit_reject(cmd, Kind::Cancel);
                }
                continue;
            }
            // The per-order verdicts come back in the body; a transport
            // failure is the only thing this callback can honestly report.
            std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
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
        const auto request = builder_.open_orders(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local OrderBuffer storage;
        const auto orders = market == MarketType::Spot
                                ? parse_spot_open_orders(*body, storage)
                                : parse_rest_orders(*body, storage, market);
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
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.query_order(id, market, state_->venue_now());
        if (!request.has_value()) {
            // Spot has no client-id lookup that does not also need the pair;
            // the open-order sweep answers the same question.
            return from_open_orders(id, market);
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_rest_order(*body, market);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto report = map_open_order(*wire, state_->instruments);
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
        const auto request = builder_.my_trades(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local UserTradeBuffer storage;
        const auto trades = parse_rest_user_trades(*body, storage, market);
        if (!trades.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(trades->size());
        for (const auto& wire : *trades) {
            const auto memo = state_->memo_for(wire.order_id);
            const auto event = map_user_trade(
                wire, state_->instruments, memo.leaves, memo.client_order_id, state_->clock->wall());
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
            // claim about a concept Gate does not have on this API.
            return std::unexpected{exec::not_implemented("gate spot position pull")};
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

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body)};
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
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind);
            return;
        }
        const auto market = cmd.instrument.market();
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
        if (response.status >= 200 && response.status < 300) {
            // The success body IS the order. Remembering it here seeds the
            // leaves before the socket's `_new` push arrives, which is what
            // lets a fill that overtakes that push still state its leaves.
            if (const auto wire = parse_rest_order(response.body, market); wire.has_value()) {
                state_->remember(*wire);
            }
            return;
        }
        if (kind == Kind::Cancel) {
            if (const auto err = parse_rest_error(response.body);
                err.has_value() && is_benign_cancel_failure(err->label)) {
                return; // the order already finished; reconciliation settles it
            }
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
    std::unique_ptr<PrivateStream> perp_;
    std::unique_ptr<PrivateStream> spot_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
    bool started_{false};
};

class GateInstruments final : public exec::InstrumentSource {
public:
    explicit GateInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)},
                                .port = "443",
                                .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        net::RestRequest request{};
        switch (market) {
        case MarketType::Perp:
            request = state_->builder.contracts(k_settle_usdt);
            break;
        case MarketType::Spot:
            request = state_->builder.currency_pairs();
            break;
        case MarketType::Future:
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "gate delivery contracts are not loaded")};
        case MarketType::Option:
            // /api/v4/options/contracts is keyed by underlying and expiry, and
            // nothing here builds an OptionInstrument out of one.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "gate options contracts are not loaded")};
        }

        rest_.start();
        sync_clock(*state_, rest_);

        const auto response =
            await_response(*state_, rest_, request, k_catalogue_timeout_ms + 5'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body)};
        }
        return market == MarketType::Spot ? load_spot(response->body) : load_perp(response->body);
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp(std::string_view body) {
        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, [](const WireContract& row) {
            return map_instrument(row, MarketType::Perp);
        });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot(std::string_view body) {
        static thread_local CurrencyPairBuffer storage;
        const auto wire = parse_currency_pairs(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, map_currency_pair);
    }

    template <typename Row, typename Map>
    std::vector<Instrument> intern(std::span<const Row> rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(state_->instruments.size() + rows.size());
        for (const auto& row : rows) {
            auto instrument = map(row);
            if (!instrument.has_value()) {
                continue; // delisting, not trading, or missing a usable step
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
};

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
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

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Gate);
    adapter.data = std::make_unique<GateData>(state);
    adapter.execution =
        std::make_unique<GateExec>(state, config.enable_spot, config.enable_perp);
    adapter.instruments = std::make_unique<GateInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Gate, &make);
}

} // namespace raikiri::venues::gate
