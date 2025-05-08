#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/coinbase/book.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/rest.hpp>
#include <raikiri/venues/coinbase/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::coinbase {

namespace {

// level2_batch's opening snapshot for BTC-USD measured 1,203,894 bytes —
// 45,805 levels in one frame — so the transport's 1 MiB default drops the
// only frame that can start a book. Sized for a book several times that
// rather than for the one measured; the value is a ceiling, not a buffer.
constexpr std::size_t k_max_message_bytes = std::size_t{8} * 1024 * 1024;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string product_id;
};

// State every coinbase connection shares within one gateway process. Single
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

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;

    std::int64_t clock_offset_ns{0};

    std::uint64_t channels_confirmed{0};
    std::uint64_t subscribe_refusals{0};
    exec::VenueError last_error{};

    void add_subscription(std::string_view channel, std::string_view product_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.product_id == product_id) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{product_id});
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

// The book's own ts_event: every Coinbase timestamp is ISO-8601 text, and an
// unreadable one falls back to ingest rather than to zero.
UnixNanos book_time(std::string_view text, UnixNanos fallback) {
    const auto parsed = parse_iso8601(text);
    return parsed.has_value() ? *parsed : fallback;
}

void publish(VenueState& state, Book& book, std::string_view time, UnixNanos received) {
    if (const auto depth = book.snapshot(book_time(time, received), received); depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Match: {
        const auto wire = parse_match(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::Snapshot: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_l2_snapshot(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->product_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state.book_for(base);
        if (book.reset(*wire) != BookResult::Resynced) {
            return;
        }
        publish(state, book, wire->time, received);
        break;
    }
    case EventKind::L2Update: {
        static thread_local ChangeBuffer changes;
        const auto wire = parse_l2_update(frame, changes);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->product_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state.book_for(base);
        if (book.apply(*wire) != BookResult::Applied) {
            return; // unsynced or dropped: silence, never a guess
        }
        publish(state, book, wire->time, received);
        break;
    }
    case EventKind::Subscriptions: {
        static thread_local AckBuffer ack;
        if (const auto channels = parse_subscriptions(frame, ack); channels.has_value()) {
            state.channels_confirmed = channels->size();
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            ++state.subscribe_refusals;
            state.last_error =
                exec::VenueError::make(classify_stream(err->message), 0, err->reason);
        }
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
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.channel = entry.channel, .product_id = entry.product_id});
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
        // A fresh snapshot is coming and it replaces the book wholesale; what
        // is held now describes a subscription that no longer exists.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
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

class CoinbaseData final : public exec::DataClient {
public:
    explicit CoinbaseData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host()},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()},
                                .max_message_bytes = k_max_message_bytes},
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
        const auto channel = channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        state_->add_subscription(channel, symbol);
        if (client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.channel = channel, .product_id = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        (void)now;
        if (!started_) {
            return;
        }
        state_->io.poll();
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

// Read-only by design, not by omission: the caps row still describes what
// Coinbase OFFERS, and this refusal describes what we chose to build.
exec::VenueError read_only() noexcept {
    return exec::VenueError::make(
        exec::ErrorClass::Terminal, 0, "coinbase is a market-data-only venue");
}

class CoinbaseExec final : public exec::ExecutionClient {
public:
    explicit CoinbaseExec(exec::GatewaySink& sink, const Clock& clock)
        : sink_{&sink}, clock_{&clock} {}

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
        event.ts_event = clock_->wall();
        event.ts_init = event.ts_event;
        sink_->on_order_event(event);
    }

    exec::GatewaySink* sink_;
    const Clock* clock_;
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
    const auto iso = parse_server_time(response->body);
    if (!iso.has_value()) {
        return;
    }
    const auto venue = parse_iso8601(*iso);
    if (!venue.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(venue->ns) - static_cast<std::int64_t>(midpoint);
}

class CoinbaseInstruments final : public exec::InstrumentSource {
public:
    explicit CoinbaseInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Spot) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "coinbase exchange lists spot only")};
        }
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response = await_response(*state_, rest_, state_->builder.products(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != 200) {
            const auto message = parse_rest_error(response->body);
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status),
                0,
                message.has_value() ? *message : std::string_view{"catalogue request refused"})};
        }

        static thread_local ProductBuffer storage;
        const auto wire = parse_products(response->body, storage);
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
                continue; // not online, disabled, or missing an increment
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
};

exec::VenueAdapter make(const exec::VenueConfig&, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Coinbase);
    adapter.data = std::make_unique<CoinbaseData>(state);
    adapter.execution = std::make_unique<CoinbaseExec>(sink, clock);
    adapter.instruments = std::make_unique<CoinbaseInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Coinbase, &make);
}

} // namespace raikiri::venues::coinbase
