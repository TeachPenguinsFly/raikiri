#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::bingx {

namespace {

// Funding is a poll, not a stream (see wire.hpp). One symbol goes out per tick,
// so the request rate is bounded by the cadence rather than by how many
// contracts the gateway watches.
constexpr std::uint64_t k_funding_poll_interval_ns = 2'000'000'000ULL;

// Owned as strings: the instrument table's views outlive nothing in
// particular.
struct OwnedSubscription {
    std::string symbol;
    std::string suffix;
};

// State every bingx connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, so nothing here
// synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // Owned here rather than by the transport: the socket re-asserts this set
    // on every open, so a reconnect and a cold start run the same path.
    std::vector<OwnedSubscription> desired;

    // Symbols the gateway asked for funding on, polled round-robin.
    std::vector<std::string> funding_symbols;
    std::size_t funding_cursor{0};
    std::uint64_t next_funding_poll_ns{0};

    // Venue clock offset from /server/time. Unused until signing lands, but
    // measured now because a wrong offset reads like a credentials problem.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused: a stream that never opened and one that
    // is merely quiet look identical from the sink.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    // Frames that arrived but did not decompress — a different fault from a
    // shape we misread, and worth telling apart.
    std::uint64_t inflate_failures{0};

    void add_subscription(std::string_view symbol, std::string_view suffix) {
        for (const auto& existing : desired) {
            if (existing.symbol == symbol && existing.suffix == suffix) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{symbol}, std::string{suffix});
    }
};

void handle_frame(VenueState& state, EventKind kind, std::string_view frame, UnixNanos received) {
    switch (kind) {
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
        // One frame can carry several prints; dropping the tail would lose real
        // volume on a busy contract.
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Depth:
        // Parsed and tested, but nothing consumes a book: BBO comes from
        // bookTicker, so a book here would be state kept for its own sake.
        break;
    case EventKind::Error: {
        // A refused subscription is terminal for the stream it names, and
        // silence is how it would otherwise present.
        if (const auto control = parse_control(frame); control.has_value()) {
            state.last_error_code = control->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarkPrice:
    case EventKind::Ping:
    case EventKind::ControlAck:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path, which is why the transport tracks none of
        // it. One message per stream — BingX has no batch form.
        for (const auto& entry : state_->desired) {
            const auto frame = state_->protocol.subscribe_frame(entry.symbol, entry.suffix);
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
            if (event->kind == EventKind::Ping) {
                // Inline, not from poll(): BingX drops a connection that
                // leaves a Ping unanswered, and poll() runs on the gateway's
                // cadence rather than the venue's.
                (void)client_->send(Protocol::pong_frame());
                return;
            }
            handle_frame(*state_, event->kind, *frame, received);
        } catch (...) {
            // The inflate buffer grows by allocating, and one frame is not
            // worth taking the process down for.
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
    Inflater inflater_;
};

class BingxData final : public exec::DataClient {
public:
    explicit BingxData(std::shared_ptr<VenueState> state)
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
            // Not a stream: BingX publishes funding over REST only.
            for (const auto& existing : state_->funding_symbols) {
                if (existing == symbol) {
                    return {};
                }
            }
            state_->funding_symbols.emplace_back(symbol);
            return {};
        }
        const auto suffix = data_type_suffix(kind);
        if (suffix.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        state_->add_subscription(symbol, suffix);
        if (client_.connected()) {
            if (const auto frame = state_->protocol.subscribe_frame(symbol, suffix);
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
        poll_funding(now);
    }

private:
    // One symbol per tick, round-robin. No keepalive goes out from here: BingX
    // drives that from its side and on_message answers it.
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_symbols.empty() || now.ns < state_->next_funding_poll_ns ||
            funding_in_flight_) {
            return;
        }
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
        const auto& symbol = state_->funding_symbols[state_->funding_cursor];
        state_->funding_cursor = (state_->funding_cursor + 1) % state_->funding_symbols.size();
        funding_in_flight_ = true;
        try {
            // send() allocates the completion handler and poll() is noexcept by
            // contract; a funding rate is not worth ending a process over.
            if (!rest_.send(builder_.premium_index(symbol),
                            [this](const net::RestResponse& response) { on_funding(response); })) {
                funding_in_flight_ = false;
            }
        } catch (...) {
            funding_in_flight_ = false;
        }
    }

    void on_funding(const net::RestResponse& response) noexcept {
        funding_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return; // the next tick retries; a missed rate is not an incident
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value() || envelope->code != k_ok) {
            return;
        }
        const auto wire = parse_premium_index(response.body);
        if (!wire.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        if (const auto funding = map_funding(*wire, state_->instruments, received);
            funding.has_value()) {
            state_->sink->on_funding(*funding);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    net::RestClient rest_;
    RequestBuilder builder_;
    bool funding_in_flight_{false};
    bool started_{false};
};

// Order entry is not implemented. Every path refuses with a terminal event, so
// an order can never sit open awaiting a verdict no code will produce.
class BingxExec final : public exec::ExecutionClient {
public:
    explicit BingxExec(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

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
            exec::ErrorClass::Terminal, 0, "bingx order entry not implemented");
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
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

class BingxInstruments final : public exec::InstrumentSource {
public:
    explicit BingxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_, builder_);

        const auto response = await_response(*state_, rest_, builder_.contracts(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        // Status before body: a 429 or 5xx carries no envelope, and calling
        // either a parse failure hides whether to try again.
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "catalogue: http error")};
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

        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(response->body, storage);
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
                continue; // not online, not this market, or an unusable step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            // The table rewrote the id to the slot that indexes it, so this
            // copy carries the id every inbound frame will use.
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
    adapter.caps = exec::caps_of(VenueId::Bingx);
    adapter.data = std::make_unique<BingxData>(state);
    adapter.execution = std::make_unique<BingxExec>(state);
    adapter.instruments = std::make_unique<BingxInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bingx, &make);
}

} // namespace raikiri::venues::bingx
