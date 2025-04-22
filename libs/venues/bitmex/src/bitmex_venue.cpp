#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitmex/bitmex_venue.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/map.hpp>
#include <raikiri/venues/bitmex/rest.hpp>
#include <raikiri/venues/bitmex/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::bitmex {

namespace {

// BitMEX answers the bare text "ping" with "pong" and drops a connection it has
// heard nothing from; the banner reports heartbeatEnabled:false, so this is ours.
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

// Owned as strings: the instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string table;
    std::string symbol;
};

// The per-instrument state the partial frames force, keyed by SymbolId.
struct SymbolState {
    QuoteBook quotes;
    InstrumentState instrument;
};

// State every bitmex connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // Owned here rather than by the transport: the socket re-asserts this set on
    // every open, so a reconnect and a cold start run the same path.
    std::vector<OwnedSubscription> desired;

    std::vector<SymbolState> symbols;

    std::uint64_t next_app_ping_ns{0};

    // A stream that was never established and one that is merely quiet look
    // identical from the sink, so the refusal is counted rather than inferred.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_status{0};

    void add_subscription(std::string_view table, std::string_view symbol) {
        for (const auto& existing : desired) {
            if (existing.table == table && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{table}, std::string{symbol});
    }

    // Grows on first sight, so a catalogue reload cannot leave the two vectors
    // disagreeing about what a slot means.
    [[nodiscard]] SymbolState& state_of(InstrumentId id) {
        const auto idx = id.symbol().raw;
        if (idx >= symbols.size()) {
            symbols.resize(static_cast<std::size_t>(idx) + 1);
        }
        return symbols[idx];
    }
};

void handle_quotes(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local QuoteBuffer quotes;
    const auto wire = parse_quotes(frame, quotes);
    if (!wire.has_value()) {
        return;
    }
    // A `partial` carries one row per pool; only the last completes the book.
    for (const auto& row : *wire) {
        const auto* instrument = state.instruments.find(row.symbol);
        if (instrument == nullptr) {
            continue;
        }
        const auto& base = base_of(*instrument);
        if (const auto bbo = state.state_of(base.id).quotes.apply(row, base, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto info = parse_frame(frame);
    if (!info.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
    }
    switch (info->table) {
    case TableKind::Quote:
        handle_quotes(state, frame, received);
        break;
    case TableKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        // Dropping the tail would lose real volume on a busy contract.
        for (const auto& row : *wire) {
            if (const auto trade = map_trade(row, state.instruments, received); trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case TableKind::Instrument: {
        const auto wire = parse_instrument_tick(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->symbol);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        if (const auto funding = state.state_of(base.id).instrument.apply(*wire, base, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case TableKind::OrderBook:
        // Parsed and tested, but nothing consumes a book: the BBO comes from
        // `quote`, so maintaining one would be state kept for its own sake.
        break;
    case TableKind::Error: {
        // Terminal for the arg it names, and silence is how it would otherwise
        // present. Nothing above the adapter can ask a socket what it refused.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_status = err->status;
            ++state.subscribe_refusals;
        }
        break;
    }
    case TableKind::SubscribeAck:
    case TableKind::Info:
    case TableKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path, which is why the transport tracks none of it.
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.table = entry.table, .symbol = entry.symbol});
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

class BitmexData final : public exec::DataClient {
public:
    explicit BitmexData(std::shared_ptr<VenueState> state)
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
        const auto table = topic_for(kind);
        if (table.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        state_->add_subscription(table, symbol);
        if (client_.connected()) {
            const std::array<Subscription, 1> one{Subscription{.table = table, .symbol = symbol}};
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

// Read-only by design, not by omission: the caps row still describes what BitMEX
// OFFERS, and this refusal describes what we chose to build.
exec::VenueError read_only() noexcept {
    return exec::VenueError::make(
        exec::ErrorClass::Terminal, 0, "bitmex is a market-data-only venue");
}

class BitmexExec final : public exec::ExecutionClient {
public:
    explicit BitmexExec(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    void submit(const OrderCmd& cmd) noexcept override { emit(cmd, OrderEventType::Rejected); }

    void cancel(const OrderCmd& cmd) noexcept override {
        emit(cmd, OrderEventType::CancelRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        return std::unexpected{read_only()};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId&, MarketType) override {
        return std::unexpected{read_only()};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos) override {
        // Empty rather than an error: this venue has no fills of ours to
        // reconcile, and a failing pull would look like a broken connection.
        return std::vector<exec::FillReport>{};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::vector<exec::PositionReport>{};
    }

private:
    void emit(const OrderCmd& cmd, OrderEventType type) noexcept {
        OrderEvent event{};
        event.type = type;
        event.side = cmd.side;
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
                                                std::uint64_t timeout_ms) {
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

class BitmexInstruments final : public exec::InstrumentSource {
public:
    explicit BitmexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        // ONE request, whatever the market: /instrument/active is the whole
        // venue in a single answer and the unauthenticated budget is per minute.
        const auto response = await_response(*state_, rest_, state_->builder.instruments(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != k_ok) {
            // The body may not parse; the status classifies either way.
            const auto detail = parse_rest_error(response->body);
            const auto name = detail.has_value() ? detail->name : std::string_view{};
            const auto message = detail.has_value() ? detail->message : std::string_view{"http"};
            return std::unexpected{exec::VenueError::make(
                classify(response->status, name), response->status, message)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_instruments(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, market);
            if (!instrument.has_value()) {
                continue; // not Open, not this market, quanto, or unpriceable
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            // The table rewrote the id to the slot that indexes it.
            loaded.push_back(*state_->instruments.find(*id));
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
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bitmex);
    adapter.data = std::make_unique<BitmexData>(state);
    adapter.execution = std::make_unique<BitmexExec>(state);
    adapter.instruments = std::make_unique<BitmexInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitmex, &make);
}

} // namespace raikiri::venues::bitmex
