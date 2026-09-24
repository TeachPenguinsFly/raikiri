#include <raikiri/config/secret.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bybit/book.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/market_data.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bybit::internal {
namespace {

// separate from the client so dispatch is testable without a socket
void handle_public_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto topic = parse_topic(frame);
    if (!topic.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    switch (topic->kind) {
    case TopicKind::Orderbook: {
        static thread_local OrderbookBuffer storage;
        const auto book = parse_orderbook(frame, storage);
        if (!book.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(book->symbol);
        if (instrument == nullptr) {
            return;
        }
        auto& sync = state.book_for(*instrument);
        const auto result = sync.apply(*book);
        if (result == SyncResult::GapDetected || result == SyncResult::BadLevel) {
            return;
        }
        const auto ts_event = ms_to_unix(book->ts_ms, received);
        if (const auto bbo = sync.bbo(ts_event, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        // skip building the snapshot when the sink does not want it
        if (!state.sink->wants_depth(base_of(*instrument).id, received)) {
            break;
        }
        if (const auto depth = sync.snapshot(ts_event, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case TopicKind::PublicTrade: {
        static thread_local TradeBuffer storage;
        const auto trades = parse_trades(frame, storage);
        if (!trades.has_value()) {
            return;
        }
        for (const auto& wire : *trades) {
            if (const auto trade = map_trade(wire, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case TopicKind::Tickers: {
        const auto ticker = parse_ticker(frame);
        if (!ticker.has_value()) {
            return;
        }
        // the memo is per instrument, so an unknown symbol is skipped
        const auto* listed = state.instruments.find(ticker->symbol);
        if (listed == nullptr) {
            break;
        }
        auto& memo = state.funding_memo[base_of(*listed).id.symbol().raw];
        if (const auto funding = map_funding(*ticker, state.instruments, memo, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*ticker, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    default:
        break; // control frames and private topics are not this socket's
    }
}

class PublicHandler final : public net::WsHandler {
public:
    PublicHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    // each socket resubscribes its own topics on open; invalidate_books drops only that socket's books
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_public_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason reason) override {
        state_->sink->on_connectivity({.state = reason == net::WsCloseReason::Requested
                                                    ? exec::ConnState::Disconnected
                                                    : exec::ConnState::Disconnected,
                                       .stream_id = 0,
                                       .ts = state_->clock->wall()});
    }

    [[nodiscard]] MarketType market() const noexcept { return market_; }

private:
    VenueState* state_;
    MarketType market_;
};

class BybitData final : public exec::DataClient {
public:
    BybitData(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, handler_{*state_, market},
          pool_{state_->io,
                net::WsConfig{.host = std::string{host(state_->testnet)},
                              .port = "443",
                              .target = std::string{public_stream_path(market)}},
                handler_,
                // bybit caps args per connection; the pool turns that into a per-socket count
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { invalidate_books(keys); }} {}

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::array<char, 64> buf{};
        const auto topic = topic_name(kind, symbol, buf, state_->book_depth);
        if (topic.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto before = state_->desired_topics.size();
        state_->add_topic(topic);
        if (state_->desired_topics.size() == before) {
            return {}; // Bbo and Depth share the `orderbook.` topic
        }
        // the pool opens another socket when none has room
        const std::array<std::string, 1> one{std::string{topic}};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bybit: connection pool is full")};
        }
        return {};
    }

    std::size_t wait(std::uint32_t timeout_ms) noexcept override {
        return started_ ? state_->io.run_for(timeout_ms) : 0;
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        pool_.poll(now);
        if (pool_.connected() && now.ns >= state_->next_app_ping_ns) {
            pool_.broadcast(state_->protocol.ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;

    // a reopen breaks only that socket's delta chains
    void invalidate_books(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            // only `orderbook.` keys name a book
            const auto topic = classify_topic(key);
            if (topic.kind != TopicKind::Orderbook) {
                continue;
            }
            const auto* instrument = state_->instruments.find(topic.symbol);
            if (instrument == nullptr) {
                continue;
            }
            // find, not book_for: a topic with no book has no gap
            const auto entry = state_->books.find(base_of(*instrument).id);
            if (entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    // called on every socket open
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> topics;
        topics.reserve(keys.size());
        for (const auto& key : keys) {
            topics.push_back(key);
        }
        // orderbook first: it carries the quote, and with paced sends the last topics wait longest
        std::ranges::stable_partition(
            topics, [](std::string_view topic) { return topic.starts_with("orderbook."); });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < topics.size()) {
            const auto frame =
                state_->protocol.subscribe_frame(std::span<const std::string_view>{topics}.subspan(
                    built, std::min(net::frame_subscriptions(), topics.size() - built)));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state,
                                                   MarketType market) {
    return std::make_unique<BybitData>(std::move(state), market);
}

} // namespace raikiri::venues::bybit::internal
