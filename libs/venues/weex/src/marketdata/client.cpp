#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
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

namespace raikiri::venues::weex::detail {

namespace {

// WEEX settles funding every one, four or eight hours depending on the
// contract, and republishes the running rate continuously; a poll a minute is
// far finer than the fact changes, and it is one call for the whole catalogue.
constexpr std::uint64_t k_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;

// How long between two sends on ONE connection. WEEX publishes no
// messages-per-second allowance for the socket at all, so this is the
// conservative spacing its neighbours use: a full 100-channel connection
// becomes four frames spread over half a second rather than one burst.
constexpr std::uint64_t k_subscribe_spacing_ns = 150ULL * 1'000'000ULL;

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
    const auto entry =
        state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    const auto ts_event = ms_to_unix(wire->event_ms, received);
    // THE QUOTE FIRST. WEEX publishes no top-of-book channel at all, so this
    // ladder is the only source of a Bbo on this venue, and it is the datum
    // every screen and every strategy gates on.
    if (const auto bbo = book.touch(ts_event, received); bbo.has_value()) {
        state.sink->on_bbo(*bbo);
    }
    if (const auto snapshot = book.snapshot(ts_event, received); snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

void handle_trades(VenueState& state,
                   std::string_view frame,
                   std::string_view symbol,
                   UnixNanos received) {
    static thread_local PrintBuffer storage;
    const auto wire = parse_trades(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    for (const auto& print : *wire) {
        if (const auto trade = map_trade(print, symbol, state.instruments, received);
            trade.has_value()) {
            state.sink->on_trade(*trade);
        }
    }
}

// The frame is dispatched from an event the CALLER already resolved: the
// keepalive has to be recognised before anything else looks at the bytes, and
// resolving the kind twice would parse every frame this venue sends twice.
void handle_frame(VenueState& state,
                  const EventInfo& event,
                  std::string_view frame,
                  UnixNanos received) {
    switch (event.kind) {
    case EventKind::Depth:
    case EventKind::DepthSnapshot:
        handle_depth(state, frame, received);
        break;
    case EventKind::Trade:
        handle_trades(state, frame, event.symbol, received);
        break;
    case EventKind::TradeSnapshot:
        // DROPPED, AND THAT IS THE POINT. The channel opens with up to twenty
        // historical prints and the live frames that follow re-send part of
        // that window: five ids came round twice across two sessions, while the
        // live stream repeated none in 1457 prints. Nothing downstream
        // deduplicates a trade, so publishing this counts those prints twice in
        // every signed-flow series that begins at a reconnect.
        break;
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
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

    // The pool is built after the handler it reports to, so the keepalive's
    // way back to it is set once rather than passed in.
    void attach(net::WsPool& pool) noexcept { pool_ = &pool; }

    // NOTHING IS RESUBSCRIBED OR INVALIDATED HERE. Each socket in the pool
    // re-asks for its own channels when it opens; replaying the whole desired
    // set on every one of them is the flood the pool exists to avoid. This
    // handler is shared by every connection and so cannot tell WHICH one
    // reopened — the books belonging to that socket are invalidated by the
    // pool's own open hook, which is handed its keys.
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return; // unknown shapes are ignored, never fatal: venues add frames
        }
        // KEEPALIVE IS SERVER-DRIVEN AND ANSWERED INLINE. WEEX pings each socket
        // roughly every 30 s and closes one that misses more than ten. A shared
        // handler cannot tell WHICH socket was pinged, so the answer goes to all
        // of them: every socket is therefore certainly answered, at a cost of
        // one small frame per socket per ping — under a message a second on a
        // full pool.
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
                net::WsConfig{.host = std::string{stream_host()},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // WEEX's own websocket page states "Maximum 100 channels per
                // connection" and "maximum 20 concurrent connections per IP";
                // the pool turns the first into a per-socket number instead of
                // the process's, and k_max_connections stays under the second.
                net::WsPoolConfig{.max_keys_per_connection = k_max_channels_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { invalidate_books(keys); }} {
        handler_.attach(pool_);
        meter_market_data(*state_, state_->rest);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->rest.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        state_->rest.stop();
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
        if (kind == exec::SubKind::Funding) {
            // NOT A CHANNEL. WEEX streams no funding at all, so the rate is a
            // REST poll driven from poll() below, and the instrument is
            // remembered rather than subscribed. The SAME request also places
            // the ticker channel: the 24-hour turnover is stated there and
            // nowhere else, and no other subscription kind a gateway sends
            // would ask for it.
            if (std::ranges::find(state_->funding_instruments, instrument) ==
                state_->funding_instruments.end()) {
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
    }

private:
    // Turns a kind and a symbol into one pool key and places it.
    //
    // A CHANNEL IS PLACED ONCE. Bbo and Depth resolve to the same @depth15, so
    // a gateway asking for both must spend one of the connection's hundred
    // rather than two — which is the difference between 533 instruments on a
    // full pool and 400.
    std::expected<void, exec::VenueError>
    place(exec::SubKind kind, std::string_view symbol, InstrumentId instrument) {
        std::array<char, 64> buf{};
        const auto channel = channel_name(kind, symbol, buf);
        if (channel.empty()) {
            return exec::DataClient::subscribe(kind, instrument);
        }
        if (!state_->add_channel(channel)) {
            return {}; // already placed on a socket
        }
        // THE POOL PLACES IT. A channel that does not fit on any existing socket
        // opens another, and only the pool's own ceiling refuses — which is said
        // out loud rather than leaving the instrument silently unwatched. A
        // refused channel is taken back out of the desired set, or a later
        // reconnect would build a frame asking for something no socket holds.
        const std::array<std::string, 1> one{std::string{channel}};
        if (!pool_.subscribe(one)) {
            state_->desired.pop_back();
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "weex: connection pool is full")};
        }
        return {};
    }

    // ONLY THE BOOKS THIS SOCKET FEEDS. A depth book is chained by exactly one
    // connection, so a socket that reopens has broken its own books' chains and
    // nobody else's; invalidating every book on any reopen would darken a whole
    // catalogue because one socket blinked. The channel is checked as well as
    // the symbol: a contract's @trade may sit on a different socket from its
    // @depth15, and only the depth channel names a chain that can break.
    //
    // Nothing is refetched. The reopened socket re-asks for its channels and
    // WEEX answers each depth subscription with a fresh `depthSnapshot`, which
    // is the seed — so the recovery costs one frame and no REST call.
    void invalidate_books(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            if (!std::string_view{key}.ends_with(k_depth_channel)) {
                continue;
            }
            const auto symbol =
                std::string_view{key}.substr(0, key.size() - k_depth_channel.size());
            const auto* instrument = state_->instruments.find(symbol);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto entry = state_->books.find(base_of(*instrument).id.packed);
                entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the quote channel is
    // ordered first here rather than at subscribe time: on a reconnect the
    // order that matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> channels;
        channels.reserve(keys.size());
        for (const auto& key : keys) {
            channels.emplace_back(key);
        }
        // THE QUOTE FIRST. Once the sends are paced, whatever goes last on a
        // full socket effectively does not go at all — and on this venue
        // @depth15 is the quote as well as the ladder, so a socket that got
        // through only its first frames still has a Bid/Ask column.
        std::ranges::stable_partition(
            channels, [](std::string_view channel) { return channel.ends_with(k_depth_channel); });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < channels.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{channels}.subspan(built));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    // One call covers every symbol, so there is no cursor and no per-symbol
    // fan-out: the whole table refreshes on each poll and only the instruments
    // somebody subscribed reach the sink.
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < state_->next_funding_poll_ns ||
            in_flight_) {
            return;
        }
        in_flight_ = true;
        try {
            if (!state_->rest.send(
                    state_->builder.premium_index(),
                    [this](const net::RestResponse& response) { on_funding(response); })) {
                in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            in_flight_ = false;
            return;
        }
        // Only a call the venue was actually asked buys the next minute.
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
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
            if (std::ranges::find(state_->funding_instruments, id) ==
                state_->funding_instruments.end()) {
                continue; // the table answers for 1022 contracts; nobody asked for this one
            }
            if (const auto funding = map_funding(row, state_->instruments, received);
                funding.has_value()) {
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

} // namespace raikiri::venues::weex::detail
