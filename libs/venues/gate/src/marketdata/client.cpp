#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::gate::detail {

namespace {

// Gate closes a connection idle for 60 s of application traffic; the app ping
// runs well inside that window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// Venues drop a client that floods the socket with SUBSCRIBE messages. One
// frame per tick is far under any of their allowances.
constexpr std::uint64_t k_subscribe_spacing_ns = 150ULL * 1'000'000ULL;

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send the pool
// refuses is left alone, because the next delta asks again.
void request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        const auto entry = owner->books.find(id.packed);
        if (entry == owner->books.end()) {
            return;
        }
        auto& target = entry->second;
        if (response.transport_failed() || response.status < 200 || response.status >= 300) {
            target.seed_failed();
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_order_book(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        if (const auto book = target.snapshot(s_to_unix(wire->update_s), response.received);
            book.has_value()) {
            owner->sink->on_depth(*book);
        }
    };
    if (state.depth_rest.send(
            state.builder.order_book(k_settle_usdt, base.exchange_symbol.view(), k_depth_limit),
            handled)) {
        book.seed_requested();
    }
}

void handle_book_update(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_update(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->contract);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    const auto entry =
        state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    // Asked before the delta is offered, so the frame that triggered the seed
    // is itself buffered behind it.
    if (book.wants_seed()) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

// A whole book off spot.order_book. No seed, no chain, no BookSync: Gate
// republishes the complete depth every interval there, so the only thing that
// can go wrong is a frame that does not parse — and that is silence, not a
// stale book presented as current.
void handle_book_snapshot(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_snapshot(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
        state.sink->on_depth(*book);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
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
        const auto wire = parse_trades(frame, trades, state.market);
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
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::BookUpdate:
        handle_book_update(state, frame, received);
        break;
    case EventKind::BookSnapshot:
        handle_book_snapshot(state, frame, received);
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::OrderUpdate:
    case EventKind::UserTrade:
    case EventKind::Pong:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break; // private kinds and control frames are not this socket's
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.channel = entry.channel, .contract = entry.contract});
        }
        // THE QUOTE FIRST. Once the sends are paced, whatever goes last on a
        // large catalogue effectively does not go at all — and book_ticker is
        // the Bid/Ask column every screen shows.
        std::ranges::stable_partition(
            views, [](const Subscription& one) { return one.channel.ends_with("book_ticker"); });
        const auto now_s = state_->now_s();
        // QUEUED, not sent. The pacing lives in poll(); see state.hpp.
        state_->pending_subscribes.clear();
        state_->next_subscribe_ns = 0;
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(built), now_s);
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            state_->pending_subscribes.emplace_back(frame->text);
            built += frame->consumed;
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

class GateData final : public exec::DataClient {
public:
    explicit GateData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{
              state_->io,
              net::WsConfig{.host = std::string{private_host(state_->testnet, state_->market)},
                            .port = std::string{stream_port()},
                            .target = std::string{private_path(state_->testnet, state_->market)}},
              handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        state_->depth_rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // One socket, one tree. Gate spells a spot pair and its perpetual
            // identically, so an id from the other market would be accepted
            // here and answered with the wrong tree's prices.
            return std::unexpected{exec::not_implemented("gate: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = channel_for(kind, state_->market);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        const auto before = state_->desired.size();
        state_->add_subscription(channel, symbol);
        if (state_->desired.size() == before) {
            return {};
        }
        if (client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.channel = channel, .contract = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one, state_->now_s());
                frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    // One SUBSCRIBE per tick, well under the venue's allowance.
    void drain_subscribes(UnixNanos now) noexcept {
        if (state_->pending_subscribes.empty() || now.ns < state_->next_subscribe_ns) {
            return;
        }
        if (!client_.connected()) {
            return; // the reconnect refills the queue
        }
        if (!client_.send(state_->pending_subscribes.front())) {
            return; // wedged socket; the reconnect retries the whole set
        }
        state_->pending_subscribes.erase(state_->pending_subscribes.begin());
        state_->next_subscribe_ns = now.ns + k_subscribe_spacing_ns;
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        drain_subscribes(now);
        if (client_.connected() && now.ns >= state_->next_app_ping_ns) {
            (void)client_.send(state_->protocol.ping_frame(now.ns / 1'000'000'000ULL));
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<GateData>(std::move(state));
}

} // namespace raikiri::venues::gate::detail
