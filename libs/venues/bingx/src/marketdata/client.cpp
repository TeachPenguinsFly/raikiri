#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bingx::detail {

namespace {

// Funding is a poll, not a stream (see wire.hpp). One symbol goes out per tick,
// so the request rate is bounded by the cadence rather than by how many
// contracts the gateway watches.
constexpr std::uint64_t k_funding_poll_interval_ns = 2'000'000'000ULL;

// How long between two subscribe sends on ONE connection. BingX names a single
// dataType per message, so a connection at its 200-topic ceiling is 200 frames
// and takes 30 seconds to ask for all of them — every connection draining in
// parallel, so that is the whole catalogue's ramp and not a multiple of it. The
// venue states no websocket message allowance at all, which is a reason to keep
// gate's spacing rather than to invent a faster one.
constexpr std::uint64_t k_subscribe_spacing_ns = 150ULL * 1'000'000ULL;

// Enough sockets for BingX's whole catalogue at 200 topics each: 1002
// perpetual contracts, measured 2026-09-07, times four dataTypes — bookTicker,
// trade, depth100@100ms, and the ticker a Funding subscription registers — is
// 4008, so twenty-one would do and twenty-four leaves room for the contracts
// BingX lists. The ceiling that would hurt to trip is the IP's sixty
// websockets (ws.hpp), and twenty-four leaves that room for the user stream
// and for a spot gateway beside this one.
constexpr std::size_t k_max_connections = 24;

// A pool key is one symbol and one dataType suffix, joined by a byte neither
// can contain. The pool stores keys as opaque strings and hands them back when
// a socket reconnects, so this is the only place that knows the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view symbol, std::string_view suffix) {
    std::string key{symbol};
    key.push_back(k_key_sep);
    key.append(suffix);
    return key;
}

void handle_frame(VenueState& state, EventKind kind, std::string_view frame, UnixNanos received) {
    switch (kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, state.market, received);
            bbo.has_value()) {
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
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, state.market, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, state.market, received);
            book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, state.market, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto control = parse_control(frame); control.has_value()) {
            state.last_error_code = control->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarkPrice:
    case EventKind::Ping:
    case EventKind::ControlAck:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE. Each socket in the pool re-asks for its own
    // keys when it opens; replaying the whole desired set on every one of them
    // is the flood the pool exists to avoid.
    //
    // AND NO BOOK IS INVALIDATED, because this venue holds none: every
    // {sym}@depth100 frame is a complete snapshot of the top levels with no
    // sequence number and no delta chain (book.hpp), so a reopened socket has
    // broken no continuity and there is nothing to reseed over REST. The pool's
    // OnConnectionOpen hook is left unused for that reason.
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = inflater_.inflate(payload);
            if (!frame.has_value()) {
                ++state_->inflate_failures;
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return; // unknown shapes are ignored, never fatal
            }
            if (event->kind == EventKind::Ping) {
                // EVERY socket. BingX sends the string "Ping" on each connection
                // every five seconds and closes one whose "Pong" stops arriving,
                // and a shared handler is not told which connection asked — so
                // pooling the venue turns one answer into one broadcast.
                pool_->broadcast(Protocol::pong_frame());
                return;
            }
            handle_frame(*state_, event->kind, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsPool& pool) noexcept { pool_ = &pool; }

private:
    VenueState* state_;
    net::WsPool* pool_{nullptr};
    net::Inflater inflater_;
};

class BingxData final : public exec::DataClient {
public:
    explicit BingxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->market)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path(state_->market)}},
                handler_,
                // BingX caps a CONNECTION at 200 topics; the pool turns that
                // into a per-socket number instead of the process's ceiling.
                net::WsPoolConfig{.max_keys_per_connection = k_max_subscriptions_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        handler_.attach(pool_);
    }

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        rest_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        rest_.stop();
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
        if (instrument.market() != state_->market) {
            // This socket is one deployment's, and BingX spells a spot pair
            // and its perpetual identically — an id from the other market
            // subscribed here would be fed the wrong tree's prints under the
            // right-looking symbol, which is worse than no data.
            return std::unexpected{exec::not_implemented("bingx: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            // Not a stream: BingX publishes funding over REST only. The
            // 24-hour window IS a stream, and this is the only kind a caller
            // asks the venue's own statistics with, so the poll and the
            // ticker subscription are registered together.
            if (const auto placed = subscribe_channel(symbol, k_ticker_channel);
                !placed.has_value()) {
                return placed;
            }
            for (const auto& existing : state_->funding_symbols) {
                if (existing == symbol) {
                    return {};
                }
            }
            state_->funding_symbols.emplace_back(symbol);
            return {};
        }
        const auto suffix = data_type_suffix(kind, state_->market);
        if (suffix.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        return subscribe_channel(symbol, suffix);
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
        // One io context serves both clients, and this is the exec client's
        // only tick.
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    std::expected<void, exec::VenueError> subscribe_channel(std::string_view symbol,
                                                            std::string_view suffix) {
        const auto before = state_->desired.size();
        state_->add_subscription(symbol, suffix);
        if (state_->desired.size() == before) {
            return {}; // refcounting lives in the gateway core; this is a set
        }
        // THE POOL PLACES IT. A key that fits on no existing socket opens
        // another, and only the pool's own ceiling refuses — which is said out
        // loud rather than leaving the instrument silently unwatched.
        const std::array<std::string, 1> one{key_of(symbol, suffix)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bingx: connection pool is full")};
        }
        return {};
    }

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the quote channel is
    // ordered first here rather than at subscribe time: on a reconnect the
    // order that matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        struct View {
            std::string_view symbol;
            std::string_view suffix;
        };

        std::vector<View> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            const auto cut = key.find(k_key_sep);
            if (cut == std::string::npos) {
                continue;
            }
            views.push_back({.symbol = std::string_view{key}.substr(0, cut),
                             .suffix = std::string_view{key}.substr(cut + 1)});
        }
        // THE QUOTE FIRST. Once the sends are paced, whatever goes last on a
        // large catalogue effectively does not go at all — and bookTicker is
        // the Bid/Ask column every screen shows.
        const auto quote = data_type_suffix(exec::SubKind::Bbo, state_->market);
        std::ranges::stable_partition(views,
                                      [quote](const View& one) { return one.suffix == quote; });

        // ONE FRAME PER TOPIC, not a batch: a BingX subscribe names a single
        // dataType and has no args array (ws.hpp), so a connection's ask is as
        // many frames as it holds keys. The frame's text points into the
        // protocol's own buffer and is overwritten by the next call, so each is
        // copied here rather than viewed.
        std::vector<std::string> frames;
        frames.reserve(views.size());
        for (const auto& one : views) {
            const auto frame = state_->protocol.subscribe_frame(one.symbol, one.suffix);
            if (!frame.has_value()) {
                continue;
            }
            frames.emplace_back(frame->text);
        }
        return frames;
    }

    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_symbols.empty() || now.ns < state_->next_funding_poll_ns ||
            funding_in_flight_) {
            return;
        }
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
        const auto& symbol = state_->funding_symbols[state_->funding_cursor];
        state_->funding_cursor = (state_->funding_cursor + 1) % state_->funding_symbols.size();
        funding_in_flight_ = true;
        try {
            if (!rest_.send(builder_.premium_index(symbol),
                            [this](const net::RestResponse& response) { on_funding(response); })) {
                funding_in_flight_ = false;
            }
        } catch (...) {
            funding_in_flight_ = false;
        }
    }

    void on_funding(const net::RestResponse& response) noexcept {
        funding_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return; // the next tick retries; a missed rate is not an incident
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value() || envelope->code != k_ok) {
            return;
        }
        const auto wire = parse_premium_index(response.body);
        if (!wire.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        if (const auto funding = map_funding(*wire, state_->instruments, received);
            funding.has_value()) {
            state_->sink->on_funding(*funding);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    RequestBuilder builder_;
    bool funding_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BingxData>(std::move(state));
}

} // namespace raikiri::venues::bingx::detail
