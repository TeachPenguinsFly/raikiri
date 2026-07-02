#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/market_data.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bitfinex::detail {

namespace {

// Enough sockets for bitfinex's whole SPOT catalogue at 36 channels each: 183
// pairs times three channels — ticker, trades and book — is 549, which fills
// fifteen sockets and leaves nine keys on a sixteenth (MEASURED: exactly that).
// The perpetual tree costs a fourth channel for `status` and needs nine sockets
// for its 79 contracts.
//
// EIGHTEEN IS WHAT THIS IP WAS MEASURED TO HOLD, not a number bitfinex states:
// eighteen simultaneous connections, opened one every four seconds, were all
// accepted, all acked and all delivering. What the venue does state is a RATE —
// no more than 20 connections a minute on api-pub — and THAT is the ceiling
// that would actually hurt, because a pool spends it in one go: every socket
// here reopens together after a venue restart, so a wave costs the whole pool
// inside one minute. Eighteen fits in the twenty with a retry to spare; a
// catalogue that outgrows it is refused out loud rather than opening a
// nineteenth.
constexpr std::size_t k_max_connections = 18;

// A pool key is one channel on one symbol, joined by a byte neither can
// contain. The pool stores keys as opaque strings and hands them back when a
// socket reconnects, so this is the only place that knows the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(ChannelKind kind, std::string_view symbol) {
    std::string key{channel_name(kind)};
    key.push_back(k_key_sep);
    key.append(symbol);
    return key;
}

// The inverse, and the only decode: two readers of a key that disagree about
// where the cut is drift apart silently. The symbol is a view into the key,
// which the pool owns for as long as the connection holds it.
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    const auto channel = key.substr(0, cut);
    // `status` carries several unrelated feeds and channel_kind() decides from
    // the subscription key rather than the name; `deriv:` is the only one this
    // adapter ever asks for.
    const auto kind = channel == channel_name(ChannelKind::DerivStatus) ? ChannelKind::DerivStatus
                                                                        : channel_kind(channel, {});
    if (kind == ChannelKind::Unknown) {
        return std::nullopt;
    }
    return Subscription{.kind = kind, .symbol = key.substr(cut + 1)};
}

// This socket's ids are gone, and only this socket's. The books it fed are
// chained by it and by no other connection, so invalidating the whole map here
// would put every instrument in the process back on a resync that this reopen
// did not break.
void forget(VenueState& state, ChannelRegistry& registry) noexcept {
    for (const auto& binding : registry.bound()) {
        if (binding.kind != ChannelKind::Book) {
            continue;
        }
        const auto* instrument = state.instruments.find(binding.symbol.view());
        if (instrument == nullptr) {
            continue;
        }
        if (const auto entry = state.books.find(base_of(*instrument).id.packed);
            entry != state.books.end()) {
            entry->second.invalidate();
        }
    }
    registry.clear();
}

void handle_subscribed(ChannelRegistry& registry, std::string_view frame) noexcept {
    const auto ack = parse_subscribed(frame);
    if (!ack.has_value()) {
        return;
    }
    const auto kind = channel_kind(ack->channel, ack->key);
    const auto text =
        kind == ChannelKind::DerivStatus ? symbol_from_deriv_key(ack->key) : ack->symbol;
    const auto symbol = SymbolText::from(text);
    if (kind == ChannelKind::Unknown || !symbol.has_value() || symbol->empty()) {
        return;
    }
    registry.bind(ack->chan_id, kind, *symbol);
}

void handle_data(VenueState& state,
                 const ChannelRegistry& registry,
                 std::string_view frame,
                 const FrameInfo& info,
                 UnixNanos received) noexcept {
    const auto* binding = registry.find(info.chan_id);
    if (binding == nullptr) {
        ++state.unbound_frames;
        return;
    }
    const auto ts_event = ms_to_unix(info.ts_ms, UnixNanos{0});
    switch (binding->kind) {
    case ChannelKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo =
                map_bbo(*wire, binding->symbol.view(), state.instruments, ts_event, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto stats = map_ticker_stats(
                *wire, binding->symbol.view(), state.instruments, ts_event, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case ChannelKind::Trades: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        if (wire->kind != TradeFrameKind::Executed) {
            return;
        }
        for (const auto& entry : wire->trades) {
            if (const auto trade =
                    map_trade(entry, binding->symbol.view(), state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case ChannelKind::DerivStatus: {
        const auto wire = parse_deriv_status(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding =
                map_funding(*wire, binding->symbol.view(), state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case ChannelKind::Book: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_book(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(binding->symbol.view());
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state.book_for(base);
        const auto result = book.apply(*wire);
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return; // unsynced or dropped: silence, never a guess
        }
        // A book frame carries no timestamp of any kind, so ingest is the only
        // honest ts_event this channel can offer.
        if (const auto depth = book.snapshot(received, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        // The fast top of book, beside the ticker's rather than instead of it.
        if (const auto bbo = book.touch(received, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case ChannelKind::Unknown:
        break;
    }
}

// noexcept, and so are the two above, although this path allocates: a book
// frame for an instrument seen for the first time grows state.books. An escape
// from an asio callback holding live market data is not something to unwind
// through, and the codebase already aborts rather than continues on a corrupt
// number (fixed_point.hpp), so failing loudly here is the consistent answer.
void handle_frame(VenueState& state,
                  ChannelRegistry& registry,
                  std::string_view frame,
                  UnixNanos received) noexcept {
    const auto info = parse_frame(frame);
    if (!info.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (info->kind) {
    case FrameKind::Data:
        handle_data(state, registry, frame, *info, received);
        break;
    case FrameKind::Subscribed:
        handle_subscribed(registry, frame);
        break;
    case FrameKind::Info:
        // Both notices reissue channel ids, and they arrive on the ONE socket
        // they concern. The pool's other connections were told nothing and
        // must keep the bindings they hold.
        if (info->code == k_info_reconnect || info->code == k_info_maintenance_end) {
            forget(state, registry);
        }
        break;
    case FrameKind::Error: {
        // 10305 lands here when a socket was asked for more channels than the
        // server it landed on serves. Counted rather than ignored: it is the
        // one signal that k_max_subscriptions_per_connection has been set above
        // what this venue actually gives.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case FrameKind::Auth:
        // The public sockets never authenticate; the execution client's does,
        // and it reads that frame on its own handler.
        break;
    case FrameKind::Heartbeat:
    case FrameKind::Unsubscribed:
    case FrameKind::Pong:
    case FrameKind::Unknown:
        break;
    }
}

// ONE CONNECTION'S HANDLER, which is why there is a pool per socket below
// rather than one pool of sixteen.
class SocketHandler final : public net::WsHandler {
public:
    SocketHandler(VenueState& state, ChannelRegistry& registry)
        : state_{&state}, registry_{&registry} {}

    // NOTHING IS RESUBSCRIBED HERE. The pool re-asks for this socket's keys
    // when it opens, and replaying the desired set on every connection is the
    // flood the pool exists to avoid.
    //
    // What does belong here is forgetting: every id this socket held died with
    // the old connection, and the books it fed have a broken delta chain.
    // Bitfinex reseeds a book channel with a snapshot on subscribe, so the
    // reopen this precedes is the resync — no REST burst is owed for it.
    void on_open() override {
        forget(*state_, *registry_);
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, *registry_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
    ChannelRegistry* registry_;
};

class BitfinexData final : public exec::DataClient {
public:
    explicit BitfinexData(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    std::expected<void, exec::VenueError> start() override {
        for (const auto& socket : sockets_) {
            socket->pool->start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        for (const auto& socket : sockets_) {
            socket->pool->stop();
        }
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return std::ranges::any_of(sockets_,
                                   [](const auto& socket) { return socket->pool->connected(); });
    }

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
        const auto channel = kind_for(kind);
        if (channel == ChannelKind::Unknown) {
            return DataClient::subscribe(kind, instrument);
        }
        if (channel == ChannelKind::DerivStatus && instrument.market() != MarketType::Perp) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto owned = SymbolText::from(symbol);
        if (!owned.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "symbol too long")};
        }
        if (!state_->add_subscription(channel, *owned)) {
            return {}; // Funding and MarkPrice are ONE `status` key, not two
        }
        return place(key_of(channel, symbol));
    }

    std::size_t wait(std::uint32_t timeout_ms) noexcept override {
        return started_ ? state_->io.run_for(timeout_ms) : 0;
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        const auto ping_due = now.ns >= state_->next_app_ping_ns;
        auto pinged = false;
        for (const auto& socket : sockets_) {
            socket->pool->poll(now);
            if (!ping_due || !socket->pool->connected()) {
                continue;
            }
            // EVERY socket. Bitfinex pushes a heartbeat on each subscribed
            // channel, so a socket that has bound one is never silent — but
            // the window before its first ack is, and that window is per
            // connection.
            socket->pool->broadcast(Protocol::ping_frame());
            pinged = true;
        }
        if (pinged) {
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        // The execution client's authenticated socket rides this timer too:
        // ExecutionClient has no poll hook, and a socket with no public
        // subscriptions receives no heartbeat to answer.
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    // ONE POOL PER SOCKET, AND THE REASON IS THE CHANNEL ID.
    //
    // net::WsPool reports every one of its connections to ONE handler, and
    // on_message says nothing about which socket a frame arrived on. The other
    // seven venues on the pool can live with that because their frames name
    // the instrument. Bitfinex's do not: a data frame is [CHAN_ID, payload],
    // and CHAN_ID means whatever the acknowledgement on THAT socket said it
    // means — see ChannelRegistry for what the venue was measured to do with
    // those numbers. A handler that cannot tell its sockets apart cannot keep
    // one registry per socket, so each connection gets its own handler, which
    // means its own pool.
    //
    // What the pool still owns, and what this would otherwise have to
    // reimplement per socket: the paced sends, the lazy open, and the reconnect
    // that re-asks for that connection's keys and nobody else's.
    struct Socket {
        explicit Socket(VenueState& state) : handler{state, registry} {}

        ChannelRegistry registry{};
        SocketHandler handler;
        std::unique_ptr<net::WsPool> pool; // last: it holds the handler above
    };

    // Places one key on the first socket with room, and opens another when
    // none has any. Only the connection ceiling refuses — said out loud rather
    // than leaving the instrument silently unwatched.
    std::expected<void, exec::VenueError> place(std::string key) {
        const std::array<std::string, 1> one{std::move(key)};
        for (const auto& socket : sockets_) {
            if (socket->pool->subscribe(one)) {
                return {};
            }
        }
        if (sockets_.size() >= k_max_connections) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bitfinex: connection pool is full")};
        }
        auto& opened = *sockets_.emplace_back(make_socket());
        if (started_) {
            opened.pool->start();
        }
        if (!opened.pool->subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bitfinex: a fresh socket refused a key")};
        }
        return {};
    }

    [[nodiscard]] std::unique_ptr<Socket> make_socket() {
        auto socket = std::make_unique<Socket>(*state_);
        socket->pool = std::make_unique<net::WsPool>(
            state_->io,
            net::WsConfig{.host = std::string{stream_host()},
                          .port = std::string{stream_port()},
                          .target = std::string{stream_path()}},
            socket->handler,
            // ONE connection, because the registry above it is one
            // connection's. The pool's own placement is what `place` does
            // across these single-socket pools instead.
            net::WsPoolConfig{.max_keys_per_connection = k_max_subscriptions_per_connection,
                              .max_connections = 1},
            [this](std::span<const std::string> keys) { return build_frames(keys); });
        return socket;
    }

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the ordering lives here
    // rather than at subscribe time: on a reconnect the order that matters is
    // the order the venue is asked in.
    //
    // ONE FRAME PER KEY — a `subscribe` object carries exactly one channel here
    // — so a connection's set costs its key count in sends.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        // THE BOOK FIRST. Once the sends are paced, whatever goes last
        // effectively does not go at all — and `book` is both the depth panel
        // and the Bid/Ask columns, which it fills at 1261 frames a minute
        // against the ticker channel's 5.
        std::ranges::stable_partition(
            views, [](const Subscription& one) { return one.kind == ChannelKind::Book; });

        std::vector<std::string> frames;
        frames.reserve(views.size() + 1);
        // BEFORE ANY SUBSCRIPTION, and on every reopen: `conf` sets how this
        // connection spells its numbers, and a socket that misses it answers
        // in a shape the parsers here do not read.
        frames.emplace_back(Protocol::conf_frame());
        for (const auto& view : views) {
            if (const auto frame = state_->protocol.subscribe_frame(view); frame.has_value()) {
                frames.emplace_back(frame->text);
            }
        }
        return frames;
    }

    std::shared_ptr<VenueState> state_;
    std::vector<std::unique_ptr<Socket>> sockets_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitfinexData>(std::move(state));
}

} // namespace raikiri::venues::bitfinex::detail
