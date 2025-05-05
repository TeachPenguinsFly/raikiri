#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/ws.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::xt {

namespace {

// XT streams no funding channel, so the rate is polled. One symbol is rotated
// per tick and the whole set is covered every period; the spacing floor keeps
// a large catalogue from bursting. Funding is a slow datum — the rate in force
// changes on a settlement boundary hours away — so a long period costs nothing.
constexpr std::uint64_t k_funding_period_ns = 10'000'000'000ULL;
constexpr std::uint64_t k_funding_min_spacing_ns = 250'000'000ULL;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string symbol;
    std::string params;
};

// State every xt connection shares within one gateway process. Single threaded
// by contract: the gateway loop is the only caller, which is why nothing here
// synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    std::vector<OwnedSubscription> desired;

    std::vector<std::string> funding_symbols;
    std::size_t funding_cursor{0};
    std::uint64_t next_funding_poll_ns{0};
    bool funding_in_flight{false};

    // Venue clock offset, measured against /public/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::string last_error;

    void add_subscription(const Topic& topic, std::string_view symbol) {
        for (const auto& existing : desired) {
            if (existing.channel == topic.channel && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(
            std::string{topic.channel}, std::string{symbol}, std::string{topic.params});
    }
};

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Trade: {
        const auto wire = parse_trade(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_ack(frame); err.has_value()) {
            state.last_error.assign(err->msg);
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::DepthUpdate:
        break;
    case EventKind::MarkPrice:
    case EventKind::Ack:
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
            views.push_back(
                {.channel = entry.channel, .symbol = entry.symbol, .params = entry.params});
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

class XtData final : public exec::DataClient {
public:
    explicit XtData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host()},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        client_.start();
        rest_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        rest_.stop();
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
        if (kind == exec::SubKind::Funding) {
            return watch_funding(symbol);
        }
        const auto topic = topic_for(kind);
        if (topic.channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        state_->add_subscription(topic, symbol);
        if (client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.channel = topic.channel, .symbol = symbol, .params = topic.params}};
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
        poll_funding(now);
    }

private:
    std::expected<void, exec::VenueError> watch_funding(std::string_view symbol) {
        for (const auto& existing : state_->funding_symbols) {
            if (existing == symbol) {
                return {};
            }
        }
        state_->funding_symbols.emplace_back(symbol);
        return {};
    }

    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_in_flight || state_->funding_symbols.empty() ||
            now.ns < state_->next_funding_poll_ns) {
            return;
        }
        const auto count = state_->funding_symbols.size();
        const auto& symbol = state_->funding_symbols[state_->funding_cursor];
        state_->funding_cursor = (state_->funding_cursor + 1) % count;
        const auto spacing = std::max(k_funding_min_spacing_ns, k_funding_period_ns / count);
        state_->next_funding_poll_ns = now.ns + spacing;

        state_->funding_in_flight = true;
        VenueState* state = state_.get();
        if (!rest_.send(builder_.funding_rate(symbol), [state](net::RestResponse&& response) {
                state->funding_in_flight = false;
                on_funding_response(*state, response);
            })) {
            state_->funding_in_flight = false;
        }
    }

    static void on_funding_response(VenueState& state, const net::RestResponse& response) noexcept {
        if (response.transport_failed()) {
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value() || envelope->return_code != k_ok) {
            return;
        }
        const auto wire = parse_funding_rate(response.body);
        if (!wire.has_value()) {
            return;
        }
        const auto received = state.clock->wall();
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    net::RestClient rest_;
    RequestBuilder builder_;
    bool started_{false};
};

// Order entry is not implemented. Every path refuses with a terminal event so
// an order can never sit open in the store waiting for a verdict that no code
// will ever produce.
class XtExec final : public exec::ExecutionClient {
public:
    explicit XtExec(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

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
            exec::ErrorClass::Terminal, 0, "xt order entry not implemented");
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

void sync_clock(VenueState& state, net::RestClient& rest, RequestBuilder& builder) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, builder.server_time(), 5'000);
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

class XtInstruments final : public exec::InstrumentSource {
public:
    explicit XtInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{
                    .host = std::string{rest_host()}, .port = "443", .timeout_ms = 30'000}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "xt spot catalogue is a different host")};
        }
        rest_.start();
        sync_clock(*state_, rest_, builder_);

        const auto response = await_response(*state_, rest_, builder_.symbol_list(), 30'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
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
        if (envelope->return_code != k_ok) {
            return std::unexpected{exec::VenueError::make(
                classify(envelope->code), numeric_code(envelope->code), envelope->msg)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_instruments(response->body, storage);
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
                continue; // not tradable, not a perp, or missing a step
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
    RequestBuilder builder_;
};

exec::VenueAdapter make(const exec::VenueConfig&, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Xt);
    adapter.data = std::make_unique<XtData>(state);
    adapter.execution = std::make_unique<XtExec>(state);
    adapter.instruments = std::make_unique<XtInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Xt, &make);
}

} // namespace raikiri::venues::xt
