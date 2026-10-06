#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/venues/weex/rest.hpp>
#include <raikiri/venues/weex/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::weex::internal {

namespace {

// one call per minute covers the whole catalogue
constexpr std::uint64_t k_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;

void handle_depth(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_depth(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->symbol);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    const auto entry = state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    const auto result = book.apply(*wire);
    auto& watch = state.book_watch[base.id.packed];
    watch.instrument = base.id;
    if (result == BookResult::Gap) {
        watch.gapped = true;
    }
    if (result != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    if (!wire->is_snapshot) {
        on_delta(watch, received.ns);
    } else {
        watch.seen_ns = received.ns;
        if (on_reseed_snapshot(watch, book_digest(book))) {
            RLOG_WARN("weex: {} book changed while its stream was silent, resubscribed", wire->symbol);
        }
    }
    const auto ts_event = ms_to_unix(wire->event_ms, UnixNanos{});
    // quote first; the ladder is weex's only bbo source
    if (const auto bbo = book.touch(ts_event, received); bbo.has_value()) {
        state.sink->on_bbo(*bbo);
    }
    // ask the sink before building the snapshot; the build is the expensive part
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto snapshot = book.snapshot(ts_event, received); snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

// the subscribe backfill is dropped, so the first live frame's prints it already held are history too
void handle_trade_snapshot(VenueState& state, std::string_view frame, std::string_view symbol) {
    static thread_local PrintBuffer storage;
    const auto wire = parse_trades(frame, storage);
    const auto* instrument = state.instruments.find(symbol);
    if (!wire.has_value() || instrument == nullptr) {
        return;
    }
    state.trade_floor_ms[base_of(*instrument).id.packed] = newest_trade_ms(*wire);
}

void handle_trades(VenueState& state, std::string_view frame, std::string_view symbol, UnixNanos received) {
    static thread_local PrintBuffer storage;
    const auto wire = parse_trades(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    std::uint64_t floor_ms = 0;
    if (const auto* instrument = state.instruments.find(symbol); instrument != nullptr) {
        if (const auto floor = state.trade_floor_ms.find(base_of(*instrument).id.packed);
            floor != state.trade_floor_ms.end()) {
            floor_ms = floor->second;
        }
    }
    for (const auto& print : *wire) {
        if (print.trade_ms <= floor_ms) {
            continue;
        }
        if (const auto trade = map_trade(print, symbol, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
    }
}

// the caller already resolved the event kind, so frames parse once
void handle_frame(VenueState& state, const EventInfo& event, std::string_view frame, UnixNanos received) {
    switch (event.kind) {
    case EventKind::Depth:
    case EventKind::DepthSnapshot:
        handle_depth(state, frame, received);
        break;
    case EventKind::Trade:
        handle_trades(state, frame, event.symbol, received);
        break;
    case EventKind::TradeSnapshot:
        handle_trade_snapshot(state, frame, event.symbol);
        break;
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_book_ticker(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received); stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error = std::string{err->msg};
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::Ping:
    case EventKind::Connected:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break; // control frames; the keepalive is answered in the handler
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // set once since the pool is built after the handler
    void attach(net::WsPool& pool) noexcept { pool_ = &pool; }

    // each pooled socket resubscribes its own channels; the pool's open hook invalidates that socket's books
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return; // unknown shapes are ignored, never fatal: venues add frames
        }
        // weex pings each socket every ~30 s and closes one that misses ten; the handler cannot tell which, so it
        // answers all
        if (event->kind == EventKind::Ping) {
            if (pool_ != nullptr) {
                pool_->broadcast(k_pong_frame);
            }
            return;
        }
        handle_frame(*state_, *event, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
    net::WsPool* pool_{nullptr};
};

class WeexData final : public exec::DataClient {
public:
    explicit WeexData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->market)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // weex: 100 channels per connection, 20 connections per ip
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { invalidate_books(keys); }} {
        handler_.attach(pool_);
        meter_market_data(*state_, state_->market_rest());
    }

    std::expected<void, exec::VenueError> start() override {
        state_->market_rest().start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        state_->market_rest().stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (state_->market == MarketType::Spot) {
            if (kind == exec::SubKind::Bbo || kind == exec::SubKind::Funding) {
                state_->spot_tickers.want(instrument);
            }
            if (kind == exec::SubKind::Funding) {
                return {}; // spot pays no funding; the poll above is its 24h window
            }
            return place(kind, symbol, instrument);
        }
        if (kind == exec::SubKind::Funding) {
            // funding is polled; the same request places the ticker channel for 24h turnover
            if (std::ranges::find(state_->funding_instruments, instrument) == state_->funding_instruments.end()) {
                state_->funding_instruments.push_back(instrument);
            }
            return place(exec::SubKind::MarkPrice, symbol, instrument);
        }
        return place(kind, symbol, instrument);
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
        poll_funding(now);
        state_->spot_tickers.poll(now, [this] {
            return state_->spot_rest.send(state_->builder.spot_tickers(),
                                          [this](const net::RestResponse& response) { on_spot_tickers(response); });
        });
        reseed_books(now);
    }

private:
    // Bbo and Depth share @depth200, so the channel is placed once
    std::expected<void, exec::VenueError> place(exec::SubKind kind, std::string_view symbol, InstrumentId instrument) {
        std::array<char, 64> buf{};
        const auto channel = channel_name(kind, symbol, buf, state_->market);
        if (channel.empty()) {
            return exec::DataClient::subscribe(kind, instrument);
        }
        if (!state_->add_channel(channel)) {
            return {}; // already placed on a socket
        }
        // a channel the pool refuses is removed from the desired set
        const std::array<std::string, 1> one{std::string{channel}};
        if (!pool_.subscribe(one)) {
            state_->desired.pop_back();
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "weex: connection pool is full")};
        }
        return {};
    }

    // invalidate only depth books on this socket; its resubscribe draws a fresh depthSnapshot
    void reseed_books(UnixNanos now) {
        if (now.ns < state_->next_reseed_scan_ns) {
            return;
        }
        state_->next_reseed_scan_ns = now.ns + 5ULL * 1'000'000'000ULL;
        for (auto& [packed, watch] : state_->book_watch) {
            const auto due = due_for_reseed(watch, now.ns);
            if (due == Reseed::None) {
                continue;
            }
            const auto symbol = state_->instruments.exchange_symbol(watch.instrument);
            if (symbol.empty()) {
                continue;
            }
            std::string key{symbol};
            key.append(k_depth_channel);
            const std::array<std::string_view, 1> one{key};
            std::vector<std::string> frames;
            if (const auto off = state_->protocol.unsubscribe_frame(one); off.has_value()) {
                frames.emplace_back(off->text);
            }
            if (const auto on = state_->protocol.subscribe_frame(one); on.has_value()) {
                frames.emplace_back(on->text);
            }
            if (frames.size() != 2 || !pool_.requeue(key, std::move(frames))) {
                continue;
            }
            if (const auto book = state_->books.find(packed); book != state_->books.end()) {
                if (due != Reseed::Gap && book->second.live()) {
                    watch.digest = book_digest(book->second);
                }
                book->second.invalidate();
            }
            if (due == Reseed::Gap) {
                RLOG_WARN("weex: {} book gapped, resubscribing", key);
            } else if (due == Reseed::Silent) {
                RLOG_INFO("weex: {} book silent for {} s, resubscribing with backoff while its "
                          "snapshot is unchanged",
                          key,
                          watch.silence_ns / 1'000'000'000ULL);
            }
        }
    }

    void invalidate_books(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            if (!std::string_view{key}.ends_with(k_depth_channel)) {
                continue;
            }
            const auto symbol = std::string_view{key}.substr(0, key.size() - k_depth_channel.size());
            const auto* instrument = state_->instruments.find(symbol);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto entry = state_->books.find(base_of(*instrument).id.packed); entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    // called on every open of that socket
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> channels;
        channels.reserve(keys.size());
        for (const auto& key : keys) {
            channels.emplace_back(key);
        }
        // quote first: on a paced full socket the last frames may never go
        std::ranges::stable_partition(channels, [](std::string_view channel) {
            return channel.ends_with(k_depth_channel) || channel.ends_with(k_book_ticker_channel);
        });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < channels.size()) {
            const auto frame = state_->protocol.subscribe_frame(std::span<const std::string_view>{channels}.subspan(
                built, std::min(net::frame_subscriptions(), channels.size() - built)));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    // one call covers every symbol; only subscribed instruments reach the sink
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < state_->next_funding_poll_ns || in_flight_) {
            return;
        }
        in_flight_ = true;
        try {
            if (!state_->rest.send(state_->builder.premium_index(),
                                   [this](const net::RestResponse& response) { on_funding(response); })) {
                in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            in_flight_ = false;
            return;
        }
        // only a sent call advances the interval
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
    }

    void on_spot_tickers(const net::RestResponse& response) noexcept {
        state_->spot_tickers.answered();
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<SpotTickerBuffer>();
        const auto rows = parse_spot_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto stats = map_ticker_stats(row, state_->instruments, received);
            if (stats.has_value() && state_->spot_tickers.wanted(stats->instrument)) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    void on_funding(const net::RestResponse& response) noexcept {
        in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local PremiumBuffer storage;
        const auto rows = parse_premium_index(response.body, storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto id = base_of(*instrument).id;
            if (std::ranges::find(state_->funding_instruments, id) == state_->funding_instruments.end()) {
                continue; // nobody asked for this one
            }
            if (const auto funding = map_funding(row, state_->instruments, received); funding.has_value()) {
                state_->sink->on_funding(*funding);
            }
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    bool started_{false};
    bool in_flight_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<WeexData>(std::move(state));
}

} // namespace raikiri::venues::weex::internal
