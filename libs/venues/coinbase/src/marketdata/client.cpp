#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/coinbase/book.hpp>
#include <raikiri/venues/coinbase/envelope.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/market_data.hpp>
#include <raikiri/venues/coinbase/rest.hpp>
#include <raikiri/venues/coinbase/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::coinbase::internal {

namespace {

// the level2_batch opening snapshot exceeds the 1 MiB transport default; this is a ceiling
constexpr std::size_t k_max_message_bytes = std::size_t{8} * 1024 * 1024;

// an unreadable timestamp falls back to ingest
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
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
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

// pool key: channel and product joined by a byte neither can contain
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view product_id) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(product_id);
    return key;
}

// the only decoder of key_of; views into the pool-owned key
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Subscription{.channel = key.substr(0, cut), .product_id = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // each socket resubscribes its own keys on open; the pool's open hook invalidates that socket's books
    void on_open() override {
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

private:
    VenueState* state_;
};

class CoinbaseData final : public exec::DataClient {
public:
    explicit CoinbaseData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host()},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()},
                              .max_message_bytes = k_max_message_bytes},
                handler_,
                // the ceiling is per connection
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
        const auto channel = channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (!state_->add_subscription(channel, symbol)) {
            return {}; // already asked for, and a second key would cost a socket's room
        }
        // the pool opens another socket when none has room
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "coinbase: connection pool is full")};
        }
        return {};
    }

    // coinbase has no application ping; the transport pings per socket
    std::size_t wait(std::uint32_t timeout_ms) noexcept override {
        return started_ ? state_->io.run_for(timeout_ms) : 0;
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        pool_.poll(now);
    }

private:
    // only this socket's level2_batch books: without a sequence they stay stale until the next snapshot
    void invalidate_books(std::span<const std::string> keys) {
        const auto depth = channel_for(exec::SubKind::Depth);
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value() || one->channel != depth) {
                continue;
            }
            const auto* instrument = state_->instruments.find(one->product_id);
            if (instrument == nullptr) {
                continue;
            }
            // find, not book_for: a key with no book has nothing stale
            if (const auto entry = state_->books.find(base_of(*instrument).id.packed);
                entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    // called on every socket open
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        // ticker first; level2_batch answers with a megabyte of ladder
        const auto quote = channel_for(exec::SubKind::Bbo);
        std::ranges::stable_partition(
            views, [quote](const Subscription& one) { return one.channel == quote; });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame =
                state_->protocol.subscribe_frame(std::span<const Subscription>{views}.subspan(
                    built, std::min(net::frame_subscriptions(), views.size() - built)));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<CoinbaseData>(std::move(state));
}

} // namespace raikiri::venues::coinbase::internal
