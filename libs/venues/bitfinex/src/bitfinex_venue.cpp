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

    std::array<OwnedSubscription, k_max_subscriptions_per_connection> desired{};
    std::size_t desired_count{0};

    // THE MAP NO OTHER ADAPTER IN THIS REPO NEEDS.
    std::array<ChannelBinding, k_max_subscriptions_per_connection> channels{};
    std::size_t bound_count{0};

    std::uint64_t next_app_ping_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
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
