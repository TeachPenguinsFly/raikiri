#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::deribit {

namespace {

// Deribit's floor is 10 s; near it keeps a quiet option contract's socket
// demonstrably alive rather than merely un-torn-down.
constexpr std::uint32_t k_heartbeat_interval_s = 15;

// Owned as a string: the instrument table's views outlive nothing here.
struct OwnedSubscription {
    std::string channel;
};

// State every deribit connection shares within one gateway process. Single
// threaded by contract, which is why nothing here synchronises.
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

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;

    std::int64_t clock_offset_ns{0};

    std::uint64_t channels_requested{0};
    std::uint64_t channels_accepted{0};
    std::int64_t last_error_code{0};

    void add_subscription(std::string_view channel) {
        for (const auto& existing : desired) {
            if (existing.channel == channel) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel});
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

void handle_book(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->instrument);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    const auto result = book.apply(*wire);
    if (result != BookResult::Applied && result != BookResult::Resynced) {
        return; // unsynced, redelivered or dropped: silence, never a guess
    }
    if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state,
                  net::WsClient& client,
                  std::string_view frame,
                  UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
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
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        // Dated futures share this channel and quote no funding at all.
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Book:
        handle_book(state, frame, received);
        break;
    case EventKind::Heartbeat:
        // An unanswered test_request costs the connection.
        if (event->needs_test_reply) {
            (void)client.send(state.protocol.test_frame().text);
        }
        break;
    case EventKind::Response: {
        const auto response = parse_response(frame);
        if (!response.has_value()) {
            return;
        }
        if (response->failed()) {
            state.last_error_code = response->error_code;
            break;
        }
        state.channels_accepted += response->accepted;
        break;
    }
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // Re-armed per connection: set_heartbeat is a session property.
        (void)client_->send(state_->protocol.set_heartbeat_frame(k_heartbeat_interval_s).text);

        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.channel = entry.channel});
        }
        state_->channels_requested = 0;
        state_->channels_accepted = 0;
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
            state_->channels_requested += frame->consumed;
        }
        // Whatever the change_id chain said before the socket dropped says
        // nothing about the snapshot Deribit is about to send.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, *client_, payload, received);
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

class DeribitData final : public exec::DataClient {
public:
    explicit DeribitData(std::shared_ptr<VenueState> state)
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
        std::array<char, k_max_channel_len> buffer{};
        const auto channel = channel_for(kind, symbol, buffer);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        const auto before = state_->desired.size();
        state_->add_subscription(channel);
        if (state_->desired.size() == before) {
            return {};
        }
        if (client_.connected()) {
            const std::array<Subscription, 1> one{Subscription{.channel = channel}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                if (client_.send(frame->text)) {
                    state_->channels_requested += frame->consumed;
                }
            }
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        (void)now; // Deribit drives its own keepalive; nothing is due on a timer
        state_->io.poll();
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

// Read-only by design, not by omission: the caps row still describes what
// Deribit OFFERS, and this refusal describes what we chose to build.
exec::VenueError read_only() noexcept {
    return exec::VenueError::make(
        exec::ErrorClass::Terminal, 0, "deribit is a market-data-only venue");
}

class DeribitExec final : public exec::ExecutionClient {
public:
    explicit DeribitExec(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

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
        return std::unexpected{exec::not_implemented("Deribit reconciliation pulls")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{exec::not_implemented("Deribit reconciliation pulls")};
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
    // Half the round trip is the usual estimate of one-way delay.
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

class DeribitInstruments final : public exec::InstrumentSource {
public:
    explicit DeribitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        std::string_view kind;
        switch (market) {
        case MarketType::Perp:
        case MarketType::Future:
            kind = "future";
            break;
        case MarketType::Option:
            kind = "option";
            break;
        case MarketType::Spot:
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "deribit spot is not modelled")};
        }

        rest_.start();
        sync_clock(*state_, rest_);

        std::vector<Instrument> loaded;
        for (const auto currency : k_currencies) {
            const auto response =
                await_response(*state_, rest_, state_->builder.instruments(currency, kind), 20'000);
            if (!response.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
            }
            if (response->transport_failed()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
            }
            const auto envelope = parse_response(response->body);
            if (!envelope.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
            }
            if (envelope->failed()) {
                return std::unexpected{exec::VenueError::make(
                    classify(envelope->error_code), envelope->error_code, envelope->error_message)};
            }

            static thread_local InstrumentBuffer storage;
            const auto wire = parse_instruments(response->body, storage);
            if (!wire.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            state_->instruments.reserve(state_->instruments.size() + wire->size());
            for (const auto& entry : *wire) {
                auto instrument = map_instrument(entry, market);
                if (!instrument.has_value()) {
                    continue; // inactive, linear, wrong market, or missing a step
                }
                const auto id = state_->instruments.add(*instrument);
                if (!id.has_value()) {
                    continue; // duplicate or table full: reported by count, not by throw
                }
                // The table rewrote the id to the slot that indexes it.
                loaded.push_back(*state_->instruments.find(*id));
            }
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
    adapter.caps = exec::caps_of(VenueId::Deribit);
    adapter.data = std::make_unique<DeribitData>(state);
    adapter.execution = std::make_unique<DeribitExec>(state);
    adapter.instruments = std::make_unique<DeribitInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Deribit, &make);
}

} // namespace raikiri::venues::deribit
