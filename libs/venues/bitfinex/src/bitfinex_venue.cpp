#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::bitfinex {

namespace {

// Bitfinex heartbeats every subscribed channel about every 15 s, so a socket
// carrying subscriptions is never silent. This covers the window before the
// first one binds.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// Live orders tracked for their leaves and our own id. Far above any sane
// working set; the cap exists so a venue incident cannot turn the map into the
// heap.
constexpr std::size_t k_max_live_orders = 8'192;

constexpr std::uint32_t k_private_stream_id = 1;

// A subscription the gateway asked for. The symbol is OWNED, not viewed: the
// instrument table's views outlive nothing in particular, and an ack's buffer
// is gone by the time the first data frame arrives. SymbolText is the domain's
// own inline carrier for exactly this, so nothing here reaches the heap.
struct OwnedSubscription {
    ChannelKind kind{ChannelKind::Unknown};
    SymbolText symbol{};
};

// What a channel id turns out to carry.
struct ChannelBinding {
    std::int64_t chan_id{0};
    ChannelKind kind{ChannelKind::Unknown};
    SymbolText symbol{};
};

// State every bitfinex connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    std::array<OwnedSubscription, k_max_subscriptions_per_connection> desired{};
    std::size_t desired_count{0};

    // THE MAP NO OTHER ADAPTER IN THIS REPO NEEDS.
    std::array<ChannelBinding, k_max_subscriptions_per_connection> channels{};
    std::size_t bound_count{0};

    std::unordered_map<std::uint64_t, Book> books;

    std::uint64_t next_app_ping_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
    std::uint64_t unbound_frames{0};

    config::Secret api_secret;
    std::string api_key;

    // The execution client's per-tick work, registered here because
    // ExecutionClient has no poll hook of its own.
    std::function<void(UnixNanos)> private_poll;

    // As much of a live order as Bitfinex states in any one frame. The
    // execution rows carry no client order id at all, so a fill can only be
    // attributed out of this. Keyed by the venue order id, the one field every
    // private frame does carry.
    std::unordered_map<std::string, OrderContext> live_orders;

    // Our own id against the `cid` we derived from it. The derivation is one
    // way — 128 bits do not fit in a day-scoped integer — so this map is the
    // only route back, and it is what lets an order Bitfinex echoes before the
    // REST answer lands still name the ULID that placed it.
    std::unordered_map<std::int64_t, ExchangeClientOrderId> by_cid;

    void remember(std::string_view order_id, const OrderContext& context) {
        if (order_id.empty()) {
            return;
        }
        auto key = std::string{order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        live_orders[std::move(key)] = context;
    }

    [[nodiscard]] OrderContext context_for(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? OrderContext{} : it->second;
    }

    void remember_cid(std::int64_t cid, const ExchangeClientOrderId& id) {
        if (by_cid.size() >= k_max_live_orders && !by_cid.contains(cid)) {
            return;
        }
        by_cid[cid] = id;
    }

    [[nodiscard]] ExchangeClientOrderId client_id_for(const WireOrder& order) const {
        const auto memo = context_for(order.id);
        if (!memo.client_order_id.empty()) {
            return memo.client_order_id;
        }
        const auto it = by_cid.find(order.cid);
        return it == by_cid.end() ? ExchangeClientOrderId{} : it->second;
    }

    [[nodiscard]] std::span<const OwnedSubscription> subscriptions() const noexcept {
        return {desired.data(), desired_count};
    }

    [[nodiscard]] std::span<ChannelBinding> bindings() noexcept {
        return {channels.data(), bound_count};
    }

    [[nodiscard]] std::span<const ChannelBinding> bindings() const noexcept {
        return {channels.data(), bound_count};
    }

    bool add_subscription(ChannelKind kind, SymbolText symbol) noexcept {
        for (const auto& existing : subscriptions()) {
            if (existing.kind == kind && existing.symbol == symbol) {
                return true; // refcounting lives in the gateway core; this is a set
            }
        }
        if (desired_count == desired.size()) {
            return false;
        }
        desired[desired_count] = OwnedSubscription{.kind = kind, .symbol = symbol};
        ++desired_count;
        return true;
    }

    void bind(std::int64_t chan_id, ChannelKind kind, SymbolText symbol) noexcept {
        const ChannelBinding entry{.chan_id = chan_id, .kind = kind, .symbol = symbol};
        for (auto& existing : bindings()) {
            if (existing.chan_id == chan_id) {
                existing = entry;
                return;
            }
        }
        if (bound_count == channels.size()) {
            return;
        }
        channels[bound_count] = entry;
        ++bound_count;
    }

    // Channel ids are reissued on a reconnect and on the coded `info` notices,
    // and the book that stood behind the old ids describes nothing now.
    void rebind_from_scratch() noexcept {
        bound_count = 0;
        for (auto& [id, book] : books) {
            book.invalidate();
        }
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }

    [[nodiscard]] const ChannelBinding* binding(std::int64_t chan_id) const noexcept {
        for (const auto& existing : bindings()) {
            if (existing.chan_id == chan_id) {
                return &existing;
            }
        }
        return nullptr;
    }
};

void handle_subscribed(VenueState& state, std::string_view frame) noexcept {
    const auto ack = parse_subscribed(frame);
    if (!ack.has_value()) {
        return;
    }
    const auto kind = channel_kind(ack->channel, ack->key);
    const auto text =
        kind == ChannelKind::DerivStatus ? symbol_from_deriv_key(ack->key) : ack->symbol;
    const auto symbol = SymbolText::from(text);
    if (kind == ChannelKind::Unknown || !symbol.has_value() || symbol->empty()) {
        return;
    }
    state.bind(ack->chan_id, kind, *symbol);
}

void handle_data(VenueState& state,
                 std::string_view frame,
                 const FrameInfo& info,
                 UnixNanos received) noexcept {
    const auto* binding = state.binding(info.chan_id);
    if (binding == nullptr) {
        ++state.unbound_frames;
        return;
    }
    const auto ts_event = ms_to_unix(info.ts_ms, UnixNanos{0});
    switch (binding->kind) {
    case ChannelKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo =
                map_bbo(*wire, binding->symbol.view(), state.instruments, ts_event, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case ChannelKind::Trades: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        if (wire->kind != TradeFrameKind::Executed) {
            return;
        }
        for (const auto& entry : wire->trades) {
            if (const auto trade =
                    map_trade(entry, binding->symbol.view(), state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case ChannelKind::DerivStatus: {
        const auto wire = parse_deriv_status(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding =
                map_funding(*wire, binding->symbol.view(), state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case ChannelKind::Book: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_book(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(binding->symbol.view());
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state.book_for(base);
        const auto result = book.apply(*wire);
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return; // unsynced or dropped: silence, never a guess
        }
        // A book frame carries no timestamp of any kind, so ingest is the only
        // honest ts_event this channel can offer.
        if (const auto depth = book.snapshot(received, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        // The fast top of book, beside the ticker's rather than instead of it.
        if (const auto bbo = book.touch(received, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case ChannelKind::Unknown:
        break;
    }
}

// noexcept, and so are the two above, because the registry allocates: binding
// a channel id copies a symbol, and `channels[id]` grows a map. This is the
// only adapter here whose inbound path can allocate at all. An escape from an
// asio callback holding live market data is not something to unwind through,
// and the codebase already aborts rather than continues on a corrupt number
// (fixed_point.hpp), so failing loudly here is the consistent answer.
void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) noexcept {
    const auto info = parse_frame(frame);
    if (!info.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (info->kind) {
    case FrameKind::Data:
        handle_data(state, frame, *info, received);
        break;
    case FrameKind::Subscribed:
        handle_subscribed(state, frame);
        break;
    case FrameKind::Info:
        if (info->code == k_info_reconnect || info->code == k_info_maintenance_end) {
            state.rebind_from_scratch();
        }
        break;
    case FrameKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case FrameKind::Auth:
        // The public socket never authenticates; the execution client's does,
        // and it reads this frame on its own handler.
        break;
    case FrameKind::Heartbeat:
    case FrameKind::Unsubscribed:
    case FrameKind::Pong:
    case FrameKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->rebind_from_scratch();
        (void)client_->send(Protocol::conf_frame());
        for (const auto& entry : state_->subscriptions()) {
            const auto frame = state_->protocol.subscribe_frame(
                {.kind = entry.kind, .symbol = entry.symbol.view()});
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
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->rebind_from_scratch();
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class BitfinexData final : public exec::DataClient {
public:
    explicit BitfinexData(std::shared_ptr<VenueState> state)
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
        const auto channel = kind_for(kind);
        if (channel == ChannelKind::Unknown) {
            return DataClient::subscribe(kind, instrument);
        }
        if (channel == ChannelKind::DerivStatus && instrument.market() != MarketType::Perp) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto owned = SymbolText::from(symbol);
        if (!owned.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "symbol too long")};
        }
        if (!state_->add_subscription(channel, *owned)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        if (client_.connected()) {
            if (const auto frame =
                    state_->protocol.subscribe_frame({.kind = channel, .symbol = symbol});
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
            (void)client_.send(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        // The execution client's authenticated socket rides this timer too:
        // ExecutionClient has no poll hook, and a socket with no public
        // subscriptions receives no heartbeat to answer.
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Answered BEFORE the instrument table is read: the table compares
// the WHOLE id, so an id for a market that was never loaded resolves to nothing
// and would otherwise be reported as an unknown instrument — blaming a stale
// catalogue for a market this adapter simply does not serve.
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("bitfinex dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("bitfinex options order entry");
    }
    return exec::not_implemented("bitfinex: unknown market");
}

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. Missing credentials are the one exception worth
// separating — that sends an operator to the config rather than to Bitfinex's
// status page.
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// `["error",10100,"apikey: invalid"]` — an array, like everything else here,
// so an error body and a data body are told apart by their first element.
std::optional<exec::VenueError> rest_error(std::string_view body) {
    if (!is_rest_error(body)) {
        return std::nullopt;
    }
    const auto err = parse_rest_error(body);
    if (!err.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err.error()));
    }
    return exec::VenueError::make(classify(err->code), err->code, err->msg);
}

// Routes one authenticated frame. Everything private rides CHANNEL 0 and is
// told apart by its tag; a tag with no channel in GatewaySink is dropped rather
// than parsed, which is what keeps the wallet and funding streams free.
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) noexcept {
    const auto info = parse_frame(frame);
    if (!info.has_value()) {
        return;
    }
    if (info->kind == FrameKind::Auth) {
        const auto ack = parse_auth_ack(frame);
        const bool ok = ack.has_value() && ack->ok;
        if (!ok && ack.has_value()) {
            state.last_error_code = ack->code;
        }
        state.sink->on_connectivity(
            {.state = ok ? exec::ConnState::Connected : exec::ConnState::AuthFailed,
             .stream_id = k_private_stream_id,
             .ts = state.clock->wall()});
        return;
    }
    if (info->kind != FrameKind::Data || info->chan_id != 0) {
        return;
    }

    const auto remember = [&state](const WireOrder& order) {
        const auto client_id = state.client_id_for(order);
        auto context = OrderContext{.client_order_id = client_id, .leaves = QtyLots::from_raw(0)};
        if (const auto report = map_open_order(order, state.instruments); report.has_value()) {
            context.leaves = report->leaves;
        }
        state.remember(order.id, context);
        return client_id;
    };

    if (info->tag == "on" || info->tag == "ou" || info->tag == "oc") {
        const auto wire = parse_order(frame);
        if (!wire.has_value()) {
            return;
        }
        // Remembered BEFORE the event is emitted: an execution for the same
        // order may already be queued behind this frame, and it can state
        // neither whose it is nor what is left out of anything else.
        const auto client_id = remember(*wire);
        if (const auto event = map_order_event(*wire, state.instruments, client_id, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    if (info->tag == "os") {
        static thread_local OrderBuffer storage;
        const auto orders = parse_orders(frame, storage);
        if (!orders.has_value()) {
            return;
        }
        // A snapshot is not a transition. It seeds the memo so the fills that
        // follow can name their order, and the reconciler asks for the state
        // through open_order_reports().
        for (const auto& wire : *orders) {
            (void)remember(wire);
        }
        return;
    }
    if (info->tag == "tu") {
        // `te` and `tu` are the SAME fill twice — `te` arrives first with the
        // fee still null and `tu` restates it once the fee is known. Only one
        // may reach the sink or every fill is counted twice, and `tu` is the
        // one that carries the fee.
        const auto wire = parse_execution(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto event = map_execution(
                *wire, state.instruments, state.context_for(wire->order_id), received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    // Three tags are deliberately silent. `te` is the fill `tu` restates.
    // `ps`/`pn`/`pu`/`pc` carry positions GatewaySink has no channel for, and
    // the reconciler pulls those over REST. `n` is the verdict on a WRITE, and
    // every write this adapter sends goes over REST and reads its own answer —
    // a socket `n` would be a verdict on an order nothing here placed.
}

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::array<char, 512> buffer{};
        const auto frame = ws_auth_frame(state_->api_key,
                                         state_->api_secret.view(),
                                         nonces_.next(state_->clock->wall().ns),
                                         buffer);
        if (frame.empty() || client_ == nullptr || !client_->send(frame)) {
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
    NonceSource nonces_;
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

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Order entry over REST, with the lifecycle arriving on the authenticated
// socket. REST is the transport because every write here answers with the
// notification that says whether it took — Bitfinex ANSWERS HTTP 200 TO A
// REFUSED ORDER, and that array's STATUS field is the only verdict there is.
// The websocket accepts the same writes at lower latency and is the obvious
// next step; it answers on the same `n` frames, so the reader below already
// handles what it would produce.
class BitfinexExec final : public exec::ExecutionClient {
public:
    BitfinexExec(std::shared_ptr<VenueState> state, MarketType market);

    ~BitfinexExec() override { state_->private_poll = nullptr; }

    std::expected<void, exec::VenueError> start() override;

    void stop() override;

    [[nodiscard]] bool connected() const noexcept override;

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        sweep_unanswered();
    }

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — because whether the venue acted is the one
    // thing this process cannot know. Reconciliation closes these.
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale :
             unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       "no answer from the venue; outcome unknown"));
            OrderEvent event{};
            event.type = OrderEventType::ReconGap;
            event.instrument = stale.instrument;
            event.client_order_id = stale.client_order_id;
            event.exchange_client_order_id = stale.exchange_client_order_id;
            event.side = stale.side;
            event.qty = stale.qty;
            // UNDEF, not zero: zero leaves would read as "nothing is working",
            // and whether anything is working is exactly what is unknown.
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, state_->clock->wall());
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
            auto report = map_open_order(wire, state_->instruments);
            if (!report.has_value()) {
                continue;
            }
            // /auth/r/orders is ACCOUNT-WIDE and names no market, so the sweep
            // for one market must drop the other's rows rather than report
            // them as belonging to the market that was asked for.
            if (report->instrument.market() != market) {
                continue;
            }
            report->exchange_client_order_id = state_->client_id_for(wire);
            state_->remember(
                wire.id,
                {.client_order_id = report->exchange_client_order_id, .leaves = report->leaves});
            reports.push_back(*report);
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        // Bitfinex publishes no read keyed by a client id, and its `cid` is a
        // derived integer this adapter cannot invert, so the open-order sweep
        // is what answers this question at all.
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
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.my_trades(market, since, state_->clock->wall());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local ExecutionBuffer storage;
        const auto fills = parse_executions(*body, storage);
        if (!fills.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(fills.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(fills->size());
        for (const auto& wire : *fills) {
            const auto event = map_execution(
                wire, state_->instruments, state_->context_for(wire.order_id), UnixNanos{});
            if (!event.has_value() || event->instrument.market() != market) {
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
            return std::unexpected{exec::not_implemented("bitfinex spot position pull")};
        }
        const auto request = builder_.positions(market, state_->clock->wall());
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
    fetch(const net::RestRequest& request);

    void fire(const OrderCmd& cmd, Kind kind) noexcept;

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind);

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

    void poll_private(UnixNanos now) noexcept;

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> stream_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
    NonceSource nonces_;
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
    std::uint64_t next_ping_ns_{0};
    bool spot_{false};
    bool perp_{false};
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

BitfinexExec::BitfinexExec(std::shared_ptr<VenueState> state, MarketType market)
    : state_{std::move(state)},
      rest_{state_->io, net::RestConfig{.host = std::string{private_rest_host()}, .port = "443"}},
      builder_{state_->api_key, state_->api_secret}, spot_{market == MarketType::Spot},
      perp_{market == MarketType::Perp} {}

std::expected<void, exec::VenueError> BitfinexExec::start() {
    rest_.start();
    // The socket only carries a lifecycle once it is authenticated, and it
    // cannot authenticate without credentials — so an unconfigured venue keeps
    // its REST pool and reports itself unconnected rather than opening a socket
    // that will be refused on every reconnect.
    if (!state_->api_key.empty() && !state_->api_secret.empty()) {
        stream_ = std::make_unique<PrivateStream>(*state_);
        stream_->client.start();
    }
    state_->private_poll = [this](UnixNanos now) { poll_private(now); };
    started_ = true;
    return {};
}

void BitfinexExec::stop() {
    state_->private_poll = nullptr;
    if (stream_) {
        stream_->client.stop();
    }
    rest_.stop();
    started_ = false;
}

bool BitfinexExec::connected() const noexcept {
    return started_ && stream_ != nullptr && stream_->client.connected() && (spot_ || perp_);
}

std::expected<std::string, exec::VenueError> BitfinexExec::fetch(const net::RestRequest& request) {
    const auto response = await_response(*state_, rest_, request);
    if (!response.has_value() || response->transport_failed()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
    }
    if (const auto failed = rest_error(response->body); failed.has_value()) {
        return std::unexpected{*failed};
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, response->status, "http error")};
    }
    return response->body;
}

void BitfinexExec::fire(const OrderCmd& cmd, Kind kind) noexcept {
    const auto now = state_->clock->wall();
    const auto request = kind == Kind::Create ? builder_.create_order(cmd, state_->instruments, now)
                         : kind == Kind::Cancel
                             ? builder_.cancel_order(cmd, state_->instruments, now)
                             : builder_.amend_order(cmd, state_->instruments, now);
    if (!request.has_value()) {
        emit_reject(cmd, kind, build_failure(request.error()));
        return;
    }
    if (kind == Kind::Create) {
        // Recorded before the request goes out: an `on` push can beat the REST
        // answer, and without the cid mapping it would arrive unattributable.
        if (const auto cid = cid_from(cmd.exchange_client_order_id); cid.has_value()) {
            state_->remember_cid(*cid, cmd.exchange_client_order_id);
        }
    }
    if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
            on_response(std::move(response), cmd, kind);
        })) {
        // Never left this process, so nothing is in doubt.
        emit_reject(
            cmd, kind, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
        return;
    }
    // ON THE WIRE AND UNANSWERED. Recorded here rather than in the callback
    // because the callback is exactly what may never run.
    (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                           .exchange_client_order_id = cmd.exchange_client_order_id,
                           .instrument = cmd.instrument,
                           .side = cmd.side,
                           .qty = cmd.qty,
                           .kind = cmd.kind,
                           .sent = state_->clock->mono()});
}

void BitfinexExec::on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
    // Any answer settles it, including a refusal: the table tracks silence.
    if (!response.transport_failed()) {
        (void)unanswered_.resolve(cmd.client_order_id);
    }
    if (response.transport_failed()) {
        // NOT A REJECTION. The request may have reached the matcher before the
        // read failed, and calling that a refusal is how a process records a
        // rejection opposite a live position.
        state_->sink->on_order_error(
            cmd.client_order_id,
            exec::VenueError::make(exec::ErrorClass::Connectivity,
                                   0,
                                   response.error.empty()
                                       ? std::string_view{"no response from venue"}
                                       : std::string_view{response.error}));
        return;
    }
    // An `["error",code,text]` body is the other shape a refusal arrives in,
    // and it names itself where the notification below would not.
    if (const auto refusal = rest_error(response.body); refusal.has_value()) {
        emit_reject(cmd, kind, *refusal);
        return;
    }
    // BITFINEX ANSWERS HTTP 200 TO A REFUSED ORDER. The notification's STATUS
    // is the verdict, and an adapter that read the status code instead would
    // report every rejection as an acceptance and leave the order open forever.
    const auto notification = parse_notification(response.body);
    if (!notification.has_value()) {
        emit_reject(
            cmd,
            kind,
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(notification.error())));
        return;
    }
    if (!notification_ok(*notification)) {
        emit_reject(cmd,
                    kind,
                    exec::VenueError::make(
                        classify(notification->code), notification->code, notification->text));
        return;
    }
    if (kind == Kind::Create && notification->has_order) {
        // The ack carries the order itself. Seeding the memo here is what lets
        // a fill that overtakes the socket's `on` push still state its leaves.
        state_->remember(notification->order.id,
                         {.client_order_id = cmd.exchange_client_order_id, .leaves = cmd.qty});
    }
    // The socket's push carries the lifecycle.
}

void BitfinexExec::poll_private(UnixNanos now) noexcept {
    if (stream_ == nullptr || now.ns < next_ping_ns_) {
        return;
    }
    // An authenticated socket with no public subscriptions receives no
    // heartbeat at all, so it has to speak first to stay open.
    if (stream_->client.connected()) {
        (void)stream_->client.send(Protocol::ping_frame());
    }
    next_ping_ns_ = now.ns + k_app_ping_interval_ns;
}

class BitfinexInstruments final : public exec::InstrumentSource {
public:
    explicit BitfinexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        if (const auto status = platform_status(); !status.has_value()) {
            return std::unexpected{status.error()};
        }

        const auto response = await_response(
            *state_, rest_, state_->builder.pair_info(market == MarketType::Perp), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto failed = as_venue_error(response->body); failed.has_value()) {
            return std::unexpected{*failed};
        }

        static thread_local PairBuffer storage;
        const auto wire = parse_pair_info(response->body, storage);
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
                continue; // paper pair, wrong market, or an unreadable row
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
    static std::optional<exec::VenueError> as_venue_error(std::string_view body) {
        if (!is_rest_error(body)) {
            return std::nullopt;
        }
        const auto err = parse_rest_error(body);
        if (!err.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err.error()));
        }
        return exec::VenueError::make(classify(err->code), err->code, err->msg);
    }

    std::expected<void, exec::VenueError> platform_status() {
        const auto response =
            await_response(*state_, rest_, state_->builder.platform_status(), 5'000);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "platform status unavailable")};
        }
        if (const auto failed = as_venue_error(response->body); failed.has_value()) {
            return std::unexpected{*failed};
        }
        const auto status = parse_platform_status(response->body);
        if (!status.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(status.error()))};
        }
        if (*status != k_platform_operative) {
            // Retryable, not Terminal: maintenance ends.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Retryable, *status, "platform in maintenance")};
        }
        return {};
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    config::DefaultSecretProvider provider;
    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state->api_key.assign(key.view());
            }
        }
    }
    if (!config.api_secret_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            (void)provider.resolve(*ref, state->api_secret);
        }
    }

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bitfinex);
    adapter.data = std::make_unique<BitfinexData>(state);
    adapter.execution = std::make_unique<BitfinexExec>(state, config.market);
    adapter.instruments = std::make_unique<BitfinexInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitfinex, &make);
}

} // namespace raikiri::venues::bitfinex
