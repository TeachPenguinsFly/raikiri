#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/binance/binance_venue.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::binance {

namespace {

// State every binance connection shares within one gateway process. Single
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
    RequestBuilder builder{signer};

    // Desired subscriptions, owned here rather than by the transport: the
    // socket re-asserts this set on every open, so a reconnect and a cold
    // start run the same path.
    std::vector<std::string> desired_streams;

    // Venue clock offset, measured against /fapi/v1/time. Unused until
    // signing lands, but measured now because a wrong offset shows up as
    // -1021 (INVALID_TIMESTAMP), which reads like a credentials problem.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused. A stream that was never established
    // and a stream that is merely quiet look identical from the sink, which
    // is why the refusal is counted rather than inferred.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    void add_stream(std::string_view stream) {
        for (const auto& existing : desired_streams) {
            if (existing == stream) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_streams.emplace_back(stream);
    }
};

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    const auto data = unwrap(frame);
    if (!data.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::AggTrade: {
        const auto wire = parse_agg_trade(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::MarkPrice: {
        const auto wire = parse_mark_price(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::DepthUpdate:
        // Parsed and tested, but nothing consumes a book yet: BBO comes from
        // bookTicker, so maintaining one here would be state kept for its
        // own sake. The chaining rule it needs is already pinned by tests.
        break;
    case EventKind::Error: {
        // A refused subscription is terminal for the stream it names, and
        // silence is exactly how it would otherwise present. Recorded here so
        // the operator interface can report it; nothing above the adapter can
        // ask a socket what it failed to subscribe to.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::OrderUpdate:
    case EventKind::AccountUpdate:
    case EventKind::ListenKeyExpired:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path: everything the gateway asked for is
        // re-sent here, which is why the transport tracks none of it.
        std::vector<std::string_view> streams;
        streams.reserve(state_->desired_streams.size());
        for (const auto& stream : state_->desired_streams) {
            streams.push_back(stream);
        }
        std::size_t sent = 0;
        while (sent < streams.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{streams}.subspan(sent));
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

class BinanceData final : public exec::DataClient {
public:
    explicit BinanceData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = "443",
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
        std::array<char, 64> buf{};
        const auto stream = stream_name(kind, symbol, buf);
        if (stream.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired_streams.size() >= k_max_streams_per_connection) {
            // One socket takes 1024 streams. Failing here beats discovering
            // it as silence on the 1025th subscription.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "stream limit reached")};
        }
        state_->add_stream(stream);
        if (client_.connected()) {
            const std::array<std::string_view, 1> one{stream};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        // No application-level ping: binance drives keepalive with websocket
        // control frames, which the transport already answers.
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
class BinanceExec final : public exec::ExecutionClient {
public:
    explicit BinanceExec(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

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
            exec::ErrorClass::Terminal, 0, "binance order entry not implemented");
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
    // Half the round trip is the usual estimate of one-way delay; the offset
    // only has to be good to a fraction of the recvWindow.
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

class BinanceInstruments final : public exec::InstrumentSource {
public:
    explicit BinanceInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response = await_response(*state_, rest_, state_->builder.instruments(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body);
            err.has_value() && err->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(err->code), err->code, err->msg)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_exchange_info(response->body, storage);
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
                continue; // not trading, not this market, or missing a filter
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
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Binance);
    adapter.data = std::make_unique<BinanceData>(state);
    adapter.execution = std::make_unique<BinanceExec>(state);
    adapter.instruments = std::make_unique<BinanceInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Binance, &make);
}

} // namespace raikiri::venues::binance
