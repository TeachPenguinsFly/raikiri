#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/rest.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::kraken::internal {

namespace {

void handle_book(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->product_id);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    const auto result = book.apply(*wire);
    if (result != BookResult::Applied && result != BookResult::Resynced) {
        return; // unsynced, redelivered or dropped: silence, never a guess
    }
    if (const auto depth = book.snapshot(ms_to_unix(wire->timestamp_ms, UnixNanos{}), received); depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::Ticker:
    case EventKind::TickerLite: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto funding = map_funding(*wire, state.instruments, received); funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received); stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
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
            if (const auto trade = map_trade(entry, state.instruments, received); trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::TradeSnapshot:
        break; // up to 100 prints, some of them days old: history, not trades
    case EventKind::Book:
        handle_book(state, frame, received);
        break;
    case EventKind::Alert:
        if (parse_stream_alert(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    case EventKind::Subscribed:
    case EventKind::Info:
    case EventKind::Unknown:
        break;
    // private frames are routed by the private handler; listed so a new feed is a compile error
    case EventKind::Challenge:
    case EventKind::OpenOrdersSnapshot:
    case EventKind::OpenOrders:
    case EventKind::Fills:
        break;
    }
}

// returns how many entries went out; the rest wait for the next flush or reconnect
std::size_t send_subscriptions(VenueState& state, net::WsClient& client, std::span<const OwnedSubscription> entries) {
    std::vector<Subscription> views;
    views.reserve(entries.size());
    for (const auto& entry : entries) {
        views.push_back({.feed = entry.feed, .product_id = entry.product_id});
    }
    std::size_t sent = 0;
    while (sent < views.size()) {
        const auto frame = state.protocol.subscribe_frame(std::span<const Subscription>{views}.subspan(
            sent, std::min(net::frame_subscriptions(), views.size() - sent)));
        if (!frame.has_value() || frame->consumed == 0 || !client.send(frame->text)) {
            break;
        }
        sent += frame->consumed;
    }
    return sent;
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->unsent.clear();
        (void)send_subscriptions(*state_, *client_, state_->desired);
        // the seq chain resets on reconnect; the next book_snapshot reseeds
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

class KrakenData final : public exec::DataClient {
public:
    explicit KrakenData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_}, client_{state_->io,
                                                               net::WsConfig{.host = std::string{stream_host()},
                                                                             .port = std::string{stream_port()},
                                                                             .target = std::string{stream_path()},
                                                                             .max_message_bytes = k_max_message_bytes},
                                                               handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        if (state_->testnet) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "kraken futures has no demo environment")};
        }
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto feed = feed_for(kind);
        if (feed.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (const auto cap = net::process_ws_limits().streams_per_socket; cap != 0 && state_->desired.size() >= cap) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        const auto before = state_->desired.size();
        state_->add_subscription(feed, symbol);
        if (state_->desired.size() == before) {
            return {};
        }
        if (client_.connected()) {
            add_subscription(state_->unsent, feed, symbol);
        }
        return {};
    }

    std::size_t wait(std::uint32_t timeout_ms) noexcept override {
        if (!started_) {
            return 0;
        }
        flush();
        return state_->io.run_for(timeout_ms);
    }

    void poll(UnixNanos) noexcept override {
        if (!started_) {
            return;
        }
        flush();
        state_->io.poll();
    }

private:
    void flush() {
        auto& unsent = state_->unsent;
        if (unsent.empty() || !client_.connected()) {
            return;
        }
        const auto sent = send_subscriptions(*state_, client_, unsent);
        unsent.erase(unsent.begin(), unsent.begin() + static_cast<std::ptrdiff_t>(sent));
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<KrakenData>(std::move(state));
}

} // namespace raikiri::venues::kraken::internal
