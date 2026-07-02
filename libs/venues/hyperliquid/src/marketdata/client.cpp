#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::hyperliquid::detail {

namespace {

// The venue closes a connection idle for 60 s and control frames do not count
// toward that timer, so the app ping runs well inside it.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto kind = parse_event(frame);
    if (!kind.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    switch (*kind) {
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
        // One taker sweeping several makers arrives as one frame.
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::AssetCtx: {
        const auto wire = parse_asset_ctx(frame);
        if (!wire.has_value()) {
            return;
        }
        // A SPOT context carries no `funding` field, so map_funding declines it
        // rather than publishing a rate this market does not have; the day's
        // turnover is on the same payload and is the whole reason spot asks.
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
    case EventKind::Book: {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(frame, storage);
        if (!wire.has_value()) {
            return;
        }
        // THE LADDER ALSO CARRIES THE QUOTE, and on this venue that is the only
        // way most of the catalogue ever gets one. `bbo` fires when the top of
        // book MOVES, so a pair nobody is quoting sends none at all: 264 of the
        // 326 spot pairs put no bid on the board in ninety seconds while their
        // books arrived on l2Book the whole time. Level zero of each side IS
        // the best bid and offer, stamped by the venue on the same frame, so
        // the same mapper reads it rather than a second one that could disagree.
        if (!wire->bids.empty() && !wire->asks.empty()) {
            const WireBbo top{.coin = wire->coin,
                              .bid_price = wire->bids.front().price,
                              .bid_qty = wire->bids.front().qty,
                              .ask_price = wire->asks.front().price,
                              .ask_qty = wire->asks.front().qty,
                              .ts_ms = wire->ts_ms};
            if (const auto bbo = map_bbo(top, state.instruments, received); bbo.has_value()) {
                state.sink->on_bbo(*bbo);
            }
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        if (parse_stream_error(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

// A pool key is one channel on one coin, joined by a byte neither can contain.
// The pool stores keys as opaque strings and hands them back when a socket
// reconnects, so this is the only place that knows the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view coin) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(coin);
    return key;
}

// The inverse, and the only decode: two readers of a key that disagree about
// where the cut is drift apart silently. Views into the key, which the pool
// owns for as long as the connection holds it.
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Subscription{.channel = key.substr(0, cut), .coin = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE. Each socket in the pool re-asks for its own
    // keys when it opens, and this one handler is shared by every connection so
    // it could not tell which of them called anyway.
    //
    // Replaying the whole desired set from here is what the previous single
    // socket did, and it is where the venue's coverage went: 1000 subscribe
    // frames pushed into a 256-slot outbound queue in one loop, the 257th
    // refused, and the loop broke — so 64 of the 250 instruments the gateway
    // announced were ever asked for, and the other 186 sat blank for the life
    // of the process. The pool paces one frame per connection per poll, so the
    // queue never holds more than the frame in flight.
    //
    // The pool is given no OnConnectionOpen either, because there is nothing
    // for it to do: every l2Book frame is a full snapshot with no action field
    // and no sequence id, this adapter keeps no book between frames, and a
    // reopened socket's next frame IS the seed.
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

class HyperliquidData final : public exec::DataClient {
public:
    explicit HyperliquidData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // 1000 a socket, and usually 1000 an IP too: both are in the
                // venues table.
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }} {}

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

    // What this venue's sockets were told no about. Counted by the
    // adapter since it was written; this is what finally reads it.
    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override {
        return state_->subscribe_refusals;
    }

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
            return {}; // Funding and MarkPrice are ONE activeAssetCtx key, not two
        }
        // THE POOL PLACES IT. A key that fits on no existing socket opens
        // another, and only the pool's own ceiling refuses — which is said out
        // loud rather than leaving the instrument silently unwatched.
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "hyperliquid: connection pool is full")};
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
            // EVERY socket. The 60 s idle timer is per connection, so pinging
            // one of the two leaves the other to be closed on schedule.
            pool_.broadcast(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the quote channel is
    // ordered first here rather than at subscribe time: on a reconnect the
    // order that matters is the order the venue is asked in.
    //
    // ONE FRAME PER KEY — the subscription object holds a single type and coin
    // — so a connection's set costs its key count in sends, which is what
    // the venues table's ws_streams bounds.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        // THE QUOTE FIRST. Once the sends are paced, whatever goes last
        // effectively does not go at all — and `bbo` is the only one of the
        // four that moves with the market: l2Book restates the whole ladder on
        // a timer whether the book changed or not, and trades replays a backlog
        // the moment it is asked for, so both of those keep a counter healthy
        // on a coin nobody is quoting.
        const auto quote = channel_for(exec::SubKind::Bbo);
        std::ranges::stable_partition(
            views, [quote](const Subscription& one) { return one.channel == quote; });

        std::vector<std::string> frames;
        frames.reserve(views.size());
        for (const auto& one : views) {
            // The builder writes into a buffer it owns and hands back a view,
            // so the copy has to happen before the next key overwrites it.
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                frames.emplace_back(*frame);
            }
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
    return std::make_unique<HyperliquidData>(std::move(state));
}

} // namespace raikiri::venues::hyperliquid::detail
