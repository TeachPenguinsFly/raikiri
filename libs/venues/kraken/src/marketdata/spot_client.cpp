#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/endpoints.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/spot.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::kraken::internal {

namespace {

constexpr std::array<std::string_view, 3> k_channels{"ticker", "trade", "book"};

std::string key_of(std::string_view channel, std::string_view symbol) {
    std::string key{channel};
    key.push_back('|');
    key.append(symbol);
    return key;
}

std::pair<std::string_view, std::string_view> split_key(std::string_view key) noexcept {
    const auto bar = key.find('|');
    return bar == std::string_view::npos ? std::pair{key, std::string_view{}}
                                         : std::pair{key.substr(0, bar), key.substr(bar + 1)};
}

// ticker first: with paced sends the last frames go out late
std::vector<std::string> build_frames(std::span<const std::string> keys) {
    std::vector<std::string> frames;
    for (const auto channel : k_channels) {
        std::vector<std::string_view> symbols;
        for (const auto& key : keys) {
            if (const auto [ch, symbol] = split_key(key); ch == channel && !symbol.empty()) {
                symbols.push_back(symbol);
            }
        }
        for (std::size_t at = 0; at < symbols.size(); at += net::frame_subscriptions()) {
            const auto n = std::min(net::frame_subscriptions(), symbols.size() - at);
            frames.push_back(
                spot_frame("subscribe", channel, std::span<const std::string_view>{symbols}.subspan(at, n)));
        }
    }
    return frames;
}

class SpotHandler final : public net::WsHandler {
public:
    explicit SpotHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override { handle(payload, received); }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsPool& pool) noexcept { pool_ = &pool; }

private:
    void handle(std::string_view frame, UnixNanos received) noexcept {
        const auto event = parse_spot_event(frame);
        if (!event.has_value()) {
            return;
        }
        switch (event->channel) {
        case SpotChannel::Ticker: {
            const auto wire = parse_spot_ticker(frame);
            if (!wire.has_value()) {
                return;
            }
            if (const auto bbo = map_spot_bbo(*wire, state_->instruments, received); bbo.has_value()) {
                state_->sink->on_bbo(*bbo);
            }
            if (const auto stats = map_spot_ticker_stats(*wire, state_->instruments, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
            break;
        }
        case SpotChannel::Trade: {
            static thread_local SpotTradeBuffer trades;
            const auto wire = parse_spot_trades(frame, trades);
            if (!wire.has_value()) {
                return;
            }
            for (const auto& entry : *wire) {
                if (const auto trade = map_spot_trade(entry, state_->instruments, received); trade.has_value()) {
                    state_->sink->on_trade(*trade);
                }
            }
            break;
        }
        case SpotChannel::Book:
            handle_book(frame, received);
            break;
        case SpotChannel::Refused:
            ++state_->subscribe_refusals;
            break;
        case SpotChannel::Ack:
        case SpotChannel::Other:
            break;
        }
    }

    void handle_book(std::string_view frame, UnixNanos received) {
        static thread_local LevelBuffer levels;
        const auto wire = parse_spot_book(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state_->instruments.find(wire->symbol);
        if (instrument == nullptr) {
            return;
        }
        auto& book = state_->spot_book_for(base_of(*instrument));
        const auto result = book.apply(*wire);
        if (result == SpotBookResult::ChecksumMismatch || result == SpotBookResult::BadLevel) {
            resync(wire->symbol);
            return;
        }
        if (result != SpotBookResult::Applied && result != SpotBookResult::Resynced) {
            return;
        }
        if (const auto depth = book.snapshot(UnixNanos{wire->ts_ns}, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    // v2 has no snapshot request, so the book is unsubscribed and subscribed again
    void resync(std::string_view symbol) {
        if (pool_ == nullptr) {
            return;
        }
        const std::array<std::string_view, 1> one{symbol};
        std::vector<std::string> frames;
        frames.push_back(spot_frame("unsubscribe", "book", one));
        frames.push_back(spot_frame("subscribe", "book", one));
        (void)pool_->requeue(key_of("book", symbol), std::move(frames));
    }

    VenueState* state_;
    net::WsPool* pool_{nullptr};
};

class KrakenSpotData final : public exec::DataClient {
public:
    explicit KrakenSpotData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_spot_stream},
                              .port = "443",
                              .target = std::string{ep::k_spot_ws},
                              .max_message_bytes = k_max_message_bytes},
                handler_,
                net::WsPoolConfig{},
                build_frames,
                [this](std::span<const std::string> keys) { invalidate(keys); }} {
        handler_.attach(pool_);
    }

    std::expected<void, exec::VenueError> start() override {
        if (state_->testnet) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "kraken spot has no demo environment")};
        }
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = spot_channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "kraken spot: connection pool is full")};
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
    }

private:
    // a reopened socket resubscribes and each of its books waits for a fresh snapshot
    void invalidate(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            const auto [channel, symbol] = split_key(key);
            if (channel != "book") {
                continue;
            }
            if (const auto* instrument = state_->instruments.find(symbol); instrument != nullptr) {
                state_->spot_book_for(base_of(*instrument)).invalidate();
            }
        }
    }

    std::shared_ptr<VenueState> state_;
    SpotHandler handler_;
    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_spot_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<KrakenSpotData>(std::move(state));
}

} // namespace raikiri::venues::kraken::internal
