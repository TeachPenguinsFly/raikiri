#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace raikiri::venues::bitfinex {

namespace {

// Bitfinex heartbeats every subscribed channel about every 15 s, so a socket
// carrying subscriptions is never silent. This covers the window before the
// first one binds.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

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

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // Desired subscriptions, owned here rather than by the transport: the
    // socket re-asserts this set on every open, so a reconnect and a cold
    // start run the same path. Bounded by the same ceiling subscribe() gates
    // on, so the storage is fixed rather than grown.
    std::array<OwnedSubscription, k_max_subscriptions_per_connection> desired{};
    std::size_t desired_count{0};

    // THE MAP NO OTHER ADAPTER IN THIS REPO NEEDS.
    //
    // Bitfinex data frames name a numeric channel id and NOTHING ELSE — no
    // symbol, no channel name. The id is minted by the venue at subscribe time
    // and is meaningful only within one connection, so this is the only route
    // from an inbound frame to an instrument, and it is rebuilt from the
    // `subscribed` acknowledgements every time the socket opens.
    //
    // A flat array scanned linearly, not a hash map: it holds one entry per
    // subscription and so never exceeds the ceiling above, at which size a
    // handful of integer compares beats hashing — and, more to the point, it
    // cannot allocate. That is what lets the whole inbound path be noexcept.
    std::array<ChannelBinding, k_max_subscriptions_per_connection> channels{};
    std::size_t bound_count{0};

    std::uint64_t next_app_ping_ns{0};

    // Subscriptions the venue refused. A stream that was never established and
    // a stream that is merely quiet look identical from the sink, which is why
    // the refusal is counted rather than inferred.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
    // Data frames for a channel id with no binding. Non-zero means an ack was
    // missed or the registry was cleared late — the one failure mode this
    // design has, made visible instead of guessed at.
    std::uint64_t unbound_frames{0};

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
        // Unchecked on purpose: the bound was just tested, and .at() would put
        // a throw inside a path this adapter declares noexcept.
        desired[desired_count] = OwnedSubscription{.kind = kind, .symbol = symbol};
        ++desired_count;
        return true;
    }

    void bind(std::int64_t chan_id, ChannelKind kind, SymbolText symbol) noexcept {
        const ChannelBinding entry{.chan_id = chan_id, .kind = kind, .symbol = symbol};
        // Rebinding an id already held is the ordinary case after a resubscribe
        // on a socket the venue kept open.
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

    // Called on every socket open, and on the coded `info` notices that
    // reissue ids without closing the socket.
    //
    // Dropping every binding is not tidiness. Bitfinex mints fresh ids per
    // connection, and the number space is small enough that an id from the
    // previous connection can be reissued for a DIFFERENT instrument on this
    // one. A surviving stale binding would publish that instrument's prices
    // under the old symbol's id — silently, since nothing about the frame
    // disagrees. Clearing before the resubscribe means the registry only ever
    // holds bindings this connection acknowledged.
    void rebind_from_scratch() noexcept { bound_count = 0; }

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
    // The status channel names its instrument inside `key` and carries no
    // `symbol` field at all.
    const auto text =
        kind == ChannelKind::DerivStatus ? symbol_from_deriv_key(ack->key) : ack->symbol;
    const auto symbol = SymbolText::from(text);
    if (kind == ChannelKind::Unknown || !symbol.has_value() || symbol->empty()) {
        // A symbol too long to carry could not have entered the instrument
        // table either, so binding it would only produce unmappable frames.
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
        // Only `te`. The snapshot is roughly four minutes of backfill served
        // on subscribe, and `tu` restates a print `te` already delivered —
        // publishing either turns the tape into something no consumer of
        // on_trade expects, and publishing `tu` doubles every volume figure on
        // the venue. Both are parsed and pinned by tests so the shapes are
        // already proven when something wants backfill.
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
    // Book is parsed and tested, but nothing consumes one yet: BBO comes from
    // the ticker channel, so maintaining a book here would be state kept for
    // its own sake.
    case ChannelKind::Book:
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
        // A reconnect request or the end of a maintenance window reissues
        // every channel id, and neither necessarily closes the socket first —
        // so the registry has to be dropped here on exactly the same reasoning
        // as on_open, or the ids arriving next would land on stale bindings.
        if (info->code == k_info_reconnect || info->code == k_info_maintenance_end) {
            state.rebind_from_scratch();
        }
        break;
    case FrameKind::Error: {
        // A refused subscription is terminal for the channel it names, and
        // silence is exactly how it would otherwise present. Recorded here so
        // the operator interface can report it; nothing above the adapter can
        // ask a socket what it failed to subscribe to.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
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
        // The ONE resubscribe path: everything the gateway asked for is
        // re-sent here, which is why the transport tracks none of it.
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
        // The bindings belong to the socket that issued them; a frame arriving
        // on the next one must never resolve through the last one's ids.
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
        // Spot pairs have no derivative status. Asking anyway earns a 10300
        // refusal per symbol, which is a lot of noise for a question whose
        // answer the catalogue already holds.
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
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

// Order entry is not implemented. Every path refuses with a terminal event so
// an order can never sit open in the store waiting for a verdict that no code
// will ever produce.
class BitfinexExec final : public exec::ExecutionClient {
public:
    explicit BitfinexExec(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    void submit(const OrderCmd& cmd) noexcept override { emit(cmd, OrderEventType::Rejected); }

    void cancel(const OrderCmd& cmd) noexcept override {
        // CancelRejected, not Rejected: the order this targets is still alive.
        emit(cmd, OrderEventType::CancelRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        return std::unexpected{unimplemented()};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId&, MarketType) override {
        return std::unexpected{unimplemented()};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos) override {
        return std::unexpected{unimplemented()};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{unimplemented()};
    }

private:
    static exec::VenueError unimplemented() noexcept {
        return exec::VenueError::make(
            exec::ErrorClass::Terminal, 0, "bitfinex order entry not implemented");
    }

    void emit(const OrderCmd& cmd, OrderEventType type) noexcept {
        OrderEvent event{};
        event.type = type;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
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

class BitfinexInstruments final : public exec::InstrumentSource {
public:
    explicit BitfinexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        // The /conf endpoints keep answering during a maintenance window, so
        // without this a gateway comes up with a full catalogue and a socket
        // that will never deliver a tick.
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
            // The table rewrote the id to the slot that indexes it, so the
            // copy handed back carries the id every inbound frame will use.
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    // Every REST answer here is an array, so an error is told from data by its
    // first element rather than by a status envelope.
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

exec::VenueAdapter make(const exec::VenueConfig&, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bitfinex);
    adapter.data = std::make_unique<BitfinexData>(state);
    adapter.execution = std::make_unique<BitfinexExec>(state);
    adapter.instruments = std::make_unique<BitfinexInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitfinex, &make);
}

} // namespace raikiri::venues::bitfinex
