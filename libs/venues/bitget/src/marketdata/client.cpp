#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bitget/book.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/market_data.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bitget::detail {

namespace {

// Bitget drops a connection idle for 30 s and does not count websocket
// control frames toward that timer, so the app ping runs inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// Bitget takes 10 messages a second on one connection. A frame every 150 ms is
// under that with room for the app ping, which spends the same allowance.
constexpr std::uint64_t k_subscribe_spacing_ns = 150ULL * 1'000'000ULL;

// Enough sockets for bitget's whole catalogue at 500 channels each: 779
// perpetuals times three channels — ticker, trade and books15, since Bbo,
// Funding and MarkPrice all name ticker — is 2337, so five would do and eight
// leaves room for the contracts bitget lists. Far under the hundred
// connections one IP is allowed, which is the ceiling that would hurt to trip.
constexpr std::size_t k_max_connections = 8;

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
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
    case EventKind::Books: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        // Terminal for the channel it names, and silence is how it would otherwise present.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::Orders:
    case EventKind::LoginAck:
        break; // the private socket's, and it never shares this handler
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break;
    }
}

// A pool key is one channel on one contract, joined by a byte neither can
// contain. The pool stores keys as opaque strings and hands them back when a
// socket reconnects, so this is the only place that knows the shape. instType
// is not carried: one process serves one product, so it is the same for every
// key on every socket.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view inst_id) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(inst_id);
    return key;
}

// The inverse, and the only decode: two readers of a key that disagree about
// where the cut is drift apart silently. Views into the key, which the pool
// owns for as long as the connection holds it.
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key, std::string_view type) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Subscription{
        .inst_type = type, .channel = key.substr(0, cut), .inst_id = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE, AND NO BOOK IS INVALIDATED. Each socket in
    // the pool re-asks for its own keys when it opens; replaying the whole
    // desired set on every one of them is the flood the pool exists to avoid.
    //
    // The pool is given no OnConnectionOpen either, because there is nothing
    // for it to do: books15 restates the whole ladder in every frame, map_book
    // drops anything whose action is not `snapshot`, and this adapter keeps no
    // book between frames. A reopened socket's first frame IS the seed, and no
    // REST reseed is owed for the contracts it carries.
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

class BitgetData final : public exec::DataClient {
public:
    explicit BitgetData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host()},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // The ceiling is a CONNECTION's, not the process's — reading
                // it as the process's left 334 of 779 perpetuals watched, and
                // ws.hpp has what one socket is actually served.
                net::WsPoolConfig{.max_keys_per_connection = k_max_subscriptions_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
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
            return {}; // Bbo, Funding and MarkPrice are ONE `ticker` key, not three
        }
        // THE POOL PLACES IT. A key that fits on no existing socket opens
        // another, and only the pool's own ceiling refuses — which is said out
        // loud rather than leaving the instrument silently unwatched.
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bitget: connection pool is full")};
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        pool_.poll(now);
        if (pool_.connected() && now.ns >= state_->next_app_ping_ns) {
            // EVERY socket. The 30 s idle timer is per connection, so pinging
            // one of the five leaves the other four to be closed on schedule.
            pool_.broadcast(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the quote channel is
    // ordered first here rather than at subscribe time: on a reconnect the
    // order that matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        const auto type = inst_type(state_->market, state_->testnet);
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key, type); one.has_value()) {
                views.push_back(*one);
            }
        }
        // THE QUOTE FIRST. Once the sends are paced, whatever goes last
        // effectively does not go at all — and `ticker` is the Bid/Ask column
        // every screen shows, the funding rate and the day's stats in one.
        const auto quote = channel_for(exec::SubKind::Bbo);
        std::ranges::stable_partition(
            views, [quote](const Subscription& one) { return one.channel == quote; });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(built));
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
    return std::make_unique<BitgetData>(std::move(state));
}

} // namespace raikiri::venues::bitget::detail
