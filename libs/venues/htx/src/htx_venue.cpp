#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::htx {

namespace {

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
    exec::InstrumentTable instruments;

    // One builder for both sockets: every caller sends the frame it just
    // built, and the two sockets never interleave inside a single-threaded
    // poll.
    Protocol protocol;

    // Owned here rather than by the transports: each socket re-asserts its own
    // set on every open, so a reconnect and a cold start run the same path.
    std::vector<OwnedSubscription> desired;
    std::vector<std::string> funding_codes;

    // Venue clock offset from /api/v1/timestamp. Unused until signing lands,
    // but measured now because a wrong offset reads like a credentials
    // problem.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused: a stream that never opened and one that
    // is merely quiet look identical from the sink.
    std::uint64_t subscribe_refusals{0};
    std::string last_error_code;

    // Frames that arrived but did not decompress — a different fault from a
    // shape we misread, and worth telling apart.
    std::uint64_t inflate_failures{0};

    void add_subscription(std::string_view code, std::string_view suffix) {
        for (const auto& existing : desired) {
            if (existing.code == code && existing.suffix == suffix) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{code}, std::string{suffix});
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
        // One frame can carry several prints; dropping the tail would lose
        // real volume on a busy contract.
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
    case EventKind::Depth:
        // Parsed and tested, but nothing consumes a book: BBO comes from the
        // bbo topic, so a book here would be state kept for its own sake.
        break;
    case EventKind::Error: {
        // A refused subscription is terminal for the topic it names, and
        // silence is how it would otherwise present. Nothing above the adapter
        // can ask a socket what it failed to subscribe to.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarketPing:
    case EventKind::NotifyPing:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break;
    }
}

// The market-data socket: bbo, trades and the step0 book.
class MarketHandler final : public net::WsHandler {
public:
    explicit MarketHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path for this socket, which is why the transport
        // tracks none of it. One message per topic; HTX has no batch form.
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
                // Inline, not from poll(): HTX drops a connection that leaves
                // a ping unanswered, and poll() runs on the gateway's cadence.
                if (const auto pong = state_->protocol.market_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
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
        // No ConnEvent from here: connectivity is one state per venue, and the
        // socket carrying prices is the one that should set it.
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
    Inflater inflater_;
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

    // The market socket decides: a venue with funding but no prices is down
    // for every purpose a caller has.
    [[nodiscard]] bool connected() const noexcept override { return market_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
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
        // Nothing goes out on a timer: HTX pings both sockets and on_message
        // answers each with the value it was given.
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

// Order entry is not implemented. Every path refuses with a terminal event so
// an order can never sit open in the store waiting for a verdict that no code
// will ever produce.
class HtxExec final : public exec::ExecutionClient {
public:
    explicit HtxExec(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

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
            exec::ErrorClass::Terminal, 0, "htx order entry not implemented");
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

class HtxInstruments final : public exec::InstrumentSource {
public:
    explicit HtxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response = await_response(*state_, rest_, RequestBuilder::contracts(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        // Status before body: an unrouted path answers with a Spring error
        // carrying no envelope, and calling that a parse failure hides whether
        // to try again.
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
                continue; // not listed, not this market, or an unusable step
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
};

exec::VenueAdapter make(const exec::VenueConfig&, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Htx);
    adapter.data = std::make_unique<HtxData>(state);
    adapter.execution = std::make_unique<HtxExec>(state);
    adapter.instruments = std::make_unique<HtxInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Htx, &make);
}

} // namespace raikiri::venues::htx
