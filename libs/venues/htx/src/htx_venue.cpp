#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::htx {

namespace {

// Live orders tracked for their leaves and their identity. Far above any sane
// working set; the cap exists so a venue incident cannot turn the map into the
// heap.
constexpr std::size_t k_max_live_orders = 8'192;

constexpr std::uint32_t k_private_stream_id = 1;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string code;
    std::string suffix;
};

// State every htx connection shares within one gateway process. Single
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

    std::vector<OwnedSubscription> desired;
    std::vector<std::string> funding_codes;

    // Venue clock offset from /api/v1/timestamp.
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::string last_error_code;

    std::uint64_t inflate_failures{0};

    // As much of a live order as HTX states in any one frame, keyed by the
    // venue order id — the only field every private frame on both surfaces
    // carries. Spot's REST match rows name no client order id at all, and a
    // fill that cannot say whose it is routes nowhere.
    struct OrderMemo {
        ClientOrderId client_order_id{};
        ExchangeClientOrderId exchange_client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };
    std::unordered_map<std::string, OrderMemo> live_orders;

    // What a perp order was actually sent as. The swap's client_order_id is an
    // int64, so the ULID the OMS asks query_order about was never on the wire;
    // this is the only place the two are tied together, and the contract code
    // beside it is the other half the single-order read demands.
    struct SwapTarget {
        std::int64_t client_order_id{0};
        std::string contract_code;
    };
    std::unordered_map<std::string, SwapTarget> swap_targets;

    void add_subscription(std::string_view code, std::string_view suffix) {
        for (const auto& existing : desired) {
            if (existing.code == code && existing.suffix == suffix) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{code}, std::string{suffix});
    }

    // HTX refuses a signature whose Timestamp is more than a few minutes from
    // ITS clock, so a laptop drifting looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(const WireOrder& wire) {
        if (wire.order_id.empty()) {
            return;
        }
        std::string key{wire.order_id};
        if (wire.state == WireOrderState::Canceled || wire.state == WireOrderState::Filled ||
            wire.state == WireOrderState::Rejected) {
            live_orders.erase(key);
            return;
        }
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        const auto* instrument = instruments.find(wire.contract_code);
        if (instrument == nullptr) {
            return;
        }
        auto& memo = live_orders[key];
        if (const auto leaves = order_leaves(wire, instruments); leaves.has_value()) {
            memo.leaves = *leaves;
        }
        const auto identity =
            map_identity(wire.client_order_id, wire.order_id, base_of(*instrument).id.market());
        if (!identity.has_value()) {
            return;
        }
        if (identity->client_order_id.valid()) {
            memo.client_order_id = identity->client_order_id;
        }
        if (!identity->exchange_client_order_id.empty()) {
            memo.exchange_client_order_id = identity->exchange_client_order_id;
        }
    }

    [[nodiscard]] OrderMemo memo_for(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? OrderMemo{} : it->second;
    }
};

void handle_frame(VenueState& state, EventKind kind, std::string_view frame, UnixNanos received) {
    switch (kind) {
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
    case EventKind::Funding: {
        const auto wire = parse_funding(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto depth = map_depth(*wire, state.instruments, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarketPing:
    case EventKind::NotifyPing:
    case EventKind::SpotPing:
    case EventKind::AuthReply:
    case EventKind::PrivateOrder:
    case EventKind::PrivateMatch:
    case EventKind::PrivatePosition:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break; // the private kinds belong to the exec sockets, not this one
    }
}

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

// The market-data socket: bbo, trades and the step0 book.
class MarketHandler final : public net::WsHandler {
public:
    explicit MarketHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& entry : state_->desired) {
            const auto frame = state_->protocol.subscribe_frame(entry.code, entry.suffix);
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
            if (event->kind == EventKind::MarketPing) {
                if (const auto pong = state_->protocol.market_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
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

// The notification socket: same host and gzip, a different dialect and a
// different keepalive spelling. Funding only — no other socket carries it.
class NotifyHandler final : public net::WsHandler {
public:
    explicit NotifyHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& code : state_->funding_codes) {
            const auto frame = state_->protocol.funding_subscribe_frame(code);
            if (!frame.has_value()) {
                continue;
            }
            if (!client_->send(frame->text)) {
                break;
            }
        }
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
                return;
            }
            if (event->kind == EventKind::NotifyPing) {
                if (const auto pong = state_->protocol.notify_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
                return;
            }
            handle_frame(*state_, event->kind, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {}

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

net::WsConfig socket_config(std::string_view path) {
    return {.host = std::string{stream_host()},
            .port = std::string{stream_port()},
            .target = std::string{path}};
}

class HtxData final : public exec::DataClient {
public:
    explicit HtxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, market_handler_{*state_},
          market_{state_->io, socket_config(market_path()), market_handler_},
          notify_handler_{*state_},
          notify_{state_->io, socket_config(notify_path()), notify_handler_} {
        market_handler_.attach(market_);
        notify_handler_.attach(notify_);
    }

    std::expected<void, exec::VenueError> start() override {
        market_.start();
        notify_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        market_.stop();
        notify_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return market_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != MarketType::Perp) {
            // api.hbdm.com is the futures tree. The spot host this adapter
            // knows is reached only for the private v2 stream — no public spot
            // market data is wired, and HTX spells the two markets
            // differently, so a spot code sent here is simply refused by the
            // venue after the subscription has been counted as successful.
            return std::unexpected{exec::not_implemented("htx spot market data")};
        }
        const auto code = state_->instruments.exchange_symbol(instrument);
        if (code.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            for (const auto& existing : state_->funding_codes) {
                if (existing == code) {
                    return {};
                }
            }
            state_->funding_codes.emplace_back(code);
            if (notify_.connected()) {
                if (const auto frame = state_->protocol.funding_subscribe_frame(code);
                    frame.has_value()) {
                    (void)notify_.send(frame->text);
                }
            }
            return {};
        }
        const auto suffix = topic_suffix(kind);
        if (suffix.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        state_->add_subscription(code, suffix);
        if (market_.connected()) {
            if (const auto frame = state_->protocol.subscribe_frame(code, suffix);
                frame.has_value()) {
                (void)market_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
    }

private:
    std::shared_ptr<VenueState> state_;
    MarketHandler market_handler_;
    net::WsClient market_;
    NotifyHandler notify_handler_;
    net::WsClient notify_;
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
    const auto response = await_response(state, rest, RequestBuilder::server_time(), 5'000);
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

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. An empty answer here would read as "the venue holds nothing".
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("htx dated futures order entry");
    case MarketType::Option:
        // HTX lists options on their own host and API, keyed by an underlying
        // and an expiry. Nothing maps an OptionInstrument onto that yet.
        return exec::not_implemented("htx options order entry");
    }
    return exec::not_implemented("htx: unknown market");
}

// Emits the lifecycle event an order frame reports, and one Filled per trade
// the same frame carries. Shared by both private sockets and by the REST reads
// that answer with orders.
void publish_order(VenueState& state, const WireOrder& wire, UnixNanos received) {
    const auto leaves = order_leaves(wire, state.instruments);
    for (const auto& fill : wire.fills) {
        // Leaves after THIS frame's trades is the best the swap states; a
        // per-trade running total would have to assume the array is ordered.
        if (const auto event = map_fill(wire,
                                        fill,
                                        state.instruments,
                                        leaves.value_or(QtyLots::from_raw(0)),
                                        received);
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
        : handler{state},
          client{state.io,
                 net::WsConfig{.host = std::string{host},
                               .port = std::string{stream_port()},
                               .target = std::string{path}},
                 handler} {
        handler.attach(client);
    }

    Handler handler;
    net::WsClient client;
};

class HtxExec final : public exec::ExecutionClient {
public:
    HtxExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)},
          perp_rest_{state_->io,
                     net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{spot_rest_host()}, .port = "443"}},
          builder_{state_->signer},
          spot_enabled_{market == MarketType::Spot} {
        if (market == MarketType::Perp) {
            perp_stream_ =
                std::make_unique<PrivateStream<SwapPrivateHandler>>(*state_, stream_host(),
                                                                    notify_path());
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
        const auto request = builder_.query_order(id, numeric, contract, market,
                                                  state_->venue_now());
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
        if (!id.has_value() || *id > static_cast<std::uint64_t>(
                                          std::numeric_limits<std::int64_t>::max())) {
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
            return std::unexpected{
                exec::VenueError::make(classify_http(response->status), response->status,
                                       "http error")};
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
        const auto request =
            builder_.cancel_batch(group, state_->instruments, state_->venue_now());
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
                const auto refusal =
                    response.transport_failed() ? exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response from venue") : rest_failure(response.body);
                for (const auto& cmd : pending) {
                    emit_reject(cmd, Kind::Cancel, refusal);
                }
            })) {
            for (const auto& cmd : group) {
                emit_reject(cmd, Kind::Cancel, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
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
        if (!rest_for(market).send(*request, [this, cmd, kind, market](
                                                 net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind, market);
            })) {
            emit_reject(cmd, kind, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
        }
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

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind,
                     MarketType market) {
        if (response.transport_failed()) {
            emit_reject(cmd, kind, exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response from venue")); // no verdict from the venue at all
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        const bool ok = response.status >= 200 && response.status < 300 && envelope.has_value() &&
                        envelope->ok;
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
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Terminal, 0, outcome->code));
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
};

class HtxInstruments final : public exec::InstrumentSource {
public:
    explicit HtxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host()},
                                .port = "443",
                                // Measured 13.3 s on a cold endpoint against a
                                // 5 s default, so the gateway refused to start
                                // with a timeout that reads as the venue being
                                // down. It warms to 0.2 s afterwards.
                                .timeout_ms = 20'000}},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{spot_rest_host()},
                                     .port = "443",
                                     .timeout_ms = 20'000}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const bool spot = market == MarketType::Spot;
        auto& rest = spot ? spot_rest_ : rest_;
        rest_.start();
        spot_rest_.start();
        sync_clock(*state_, rest_);

        const auto request = spot ? RequestBuilder::spot_symbols() : RequestBuilder::contracts();
        const auto response = await_response(*state_, rest, request, 20'000);
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
        if (!envelope->ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), 0, envelope->msg)};
        }
        return spot ? load_spot(response->body) : load_perp(response->body, market);
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp(std::string_view body,
                                                                       MarketType market) {
        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, [market](const WireContract& row) {
            return map_instrument(row, market);
        });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot(std::string_view body) {
        static thread_local SpotSymbolBuffer storage;
        const auto wire = parse_spot_symbols(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, map_spot_symbol);
    }

    template <typename Row, typename Map>
    std::vector<Instrument> intern(std::span<const Row> rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(state_->instruments.size() + rows.size());
        for (const auto& row : rows) {
            auto instrument = map(row);
            if (!instrument.has_value()) {
                continue; // not listed, not this market, or an unusable step
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
    adapter.caps = exec::caps_of(VenueId::Htx);
    adapter.data = std::make_unique<HtxData>(state);
    adapter.execution =
        std::make_unique<HtxExec>(state, config.market);
    adapter.instruments = std::make_unique<HtxInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Htx, &make);
}

} // namespace raikiri::venues::htx
