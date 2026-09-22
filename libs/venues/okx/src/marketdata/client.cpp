#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::okx::internal {

namespace {

// OKX drops a connection with no application message for 30 s and does not
// count websocket control frames toward that timer, so the app ping runs well
// inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
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
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::FundingRate: {
        const auto wire = parse_funding_rate(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto* listed = state.instruments.find(wire->inst_id);
        if (listed == nullptr) {
            break; // nothing to accumulate into, and nothing to publish
        }
        auto& memo = state.funding_memo[base_of(*listed).id.symbol().raw];
        if (const auto funding = map_funding(*wire, state.instruments, memo, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Tickers: {
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
    case EventKind::Books: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->inst_id);
        if (instrument == nullptr) {
            return;
        }
        auto& book = state.book_for(*instrument);
        const auto result = book.apply(*wire);
        if (result == BookResult::GapDetected || result == BookResult::BadLevel) {
            return; // poll() resubscribes for a snapshot; until then we publish nothing
        }
        // ASKED BEFORE THE SNAPSHOT IS BUILT, which is the expensive half: a
        // BookSnapshot is 3.2 KB of arrays value-initialised and filled, and
        // the cell it lands in keeps only the latest value. Measured on
        // kucoin, 2026-09-19: the same guard took that gateway from 44.5% to
        // 37.6% of a core.
        if (!state.sink->wants_depth(base_of(*instrument).id, received)) {
            break;
        }
        if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
            depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarkPrice: {
        const auto wire = parse_mark_price(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto* listed = state.instruments.find(wire->inst_id);
        if (listed == nullptr) {
            break;
        }
        auto& memo = state.funding_memo[base_of(*listed).id.symbol().raw];
        if (const auto funding = map_mark_price(*wire, state.instruments, memo, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::LoginAck:
    case EventKind::Orders:
    case EventKind::Unknown:
        break; // control frames, and the private topics of the other socket
    }
}

// A pool key is one channel on one instrument, joined by a byte neither can
// contain. The pool stores keys as opaque strings and hands them back when a
// socket reconnects, so this is the only place that knows the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view inst_id) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(inst_id);
    return key;
}

// The inverse, and the only decode: the frame builder and the reopen hook read
// the same key and must not disagree about where the cut is. Views into the
// key, which the pool owns for as long as the connection holds it.
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Subscription{.channel = key.substr(0, cut), .inst_id = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED OR INVALIDATED HERE. Each socket in the pool
    // re-asks for its own keys when it opens; replaying the whole desired set
    // on every one of them is the flood the pool exists to avoid. This handler
    // is shared by every connection and so cannot tell WHICH one reopened —
    // the books that reopen actually broke are invalidated by the pool's own
    // open hook, which is handed that socket's keys.
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

class OkxData final : public exec::DataClient {
public:
    explicit OkxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // OKX caps a CONNECTION at 1024 subscriptions; the pool turns
                // that into a per-socket number instead of the process's,
                // which is what left 171 of 471 instruments watched.
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
        if (kind == exec::SubKind::Funding) {
            // The turnover has no SubKind of its own and does not ride the
            // funding-rate frame the way it does on bybit and gate, so it is
            // opened beside it — otherwise nothing ever subscribes `tickers`
            // and every OKX row on the board keeps an empty volume cell. It
            // is opened FIRST because it is the one of the three a spot
            // instrument gets.
            if (!open(k_tickers_channel, symbol)) {
                return pool_full();
            }
            // A SPOT PAIR PAYS NO FUNDING, and asking anyway is not free.
            // Measured 2026-09-08 against ws.okx.com: `funding-rate` on
            // instId BTC-USDT is refused with 60018, and `mark-price` on the
            // same id answers instType MARGIN — a margin mark that
            // FundingMemo then publishes as a FundingUpdate carrying no rate.
            // The two cost a third of the pool, which is what left 382 of
            // 1406 spot instruments unwatched.
            if (state_->spot_enabled) {
                return {};
            }
            if (!open(channel, symbol)) {
                return pool_full();
            }
            // And the mark: OKX states it on a channel of its own,
            // `FundingUpdate` carries both, and nothing else opens it.
            // THIS IS WHY A SWAP INSTRUMENT COSTS SIX CHANNELS AND NOT FOUR.
            if (!open(channel_for(exec::SubKind::MarkPrice), symbol)) {
                return pool_full();
            }
            return {};
        }
        if (!open(channel, symbol)) {
            return pool_full();
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
            // EVERY socket. OKX times out a connection that says nothing for
            // 30 s, so a ping on one of six leaves the other five idle.
            pool_.broadcast(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        resync_books(now);
    }

private:
    [[nodiscard]] static std::unexpected<exec::VenueError> pool_full() {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "okx: connection pool is full")};
    }

    // Records the subscription and hands the key to the pool, which places it
    // on a socket with room or opens another. False only at the pool's own
    // ceiling, where the key would be carried by nothing.
    [[nodiscard]] bool open(std::string_view channel, std::string_view symbol) {
        const auto before = state_->desired.size();
        state_->add_subscription(channel, symbol);
        if (state_->desired.size() == before) {
            return true; // already placed on a socket; a key is asked for once
        }
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        return pool_.subscribe(one);
    }

    // ONLY THE BOOKS THIS SOCKET FEEDS. A book is chained on seqId by exactly
    // one connection, so a socket that reopens has broken its own books' chains
    // and nobody else's; blanking every book on any reopen would silence the
    // 300-odd instruments the other sockets are still feeding until they
    // happen to reconnect too. The channel is checked as well as the
    // instrument: bbo-tbt for a symbol may sit on a different socket from its
    // `books`, and only `books` names a chain that can break.
    void invalidate_books(std::span<const std::string> keys) {
        const auto depth = channel_for(exec::SubKind::Depth);
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value() || one->channel != depth) {
                continue;
            }
            const auto* instrument = state_->instruments.find(one->inst_id);
            if (instrument == nullptr) {
                continue;
            }
            // find, not book_for: a key with no book yet has no broken chain
            // either, and seeding one here would invent a book nothing feeds.
            if (const auto entry = state_->books.find(base_of(*instrument).id);
                entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    // A broken chain is mended by unsubscribing and subscribing its `books`
    // key on the socket that carries it, which OKX answers with a snapshot.
    void resync_books(UnixNanos now) {
        if (now.ns < state_->next_resync_scan_ns) {
            return;
        }
        state_->next_resync_scan_ns = now.ns + 1'000'000'000ULL;
        const auto depth = channel_for(exec::SubKind::Depth);
        for (auto& [id, book] : state_->books) {
            if (!book.resync_due(now.ns)) {
                continue;
            }
            const auto symbol = state_->instruments.exchange_symbol(id);
            if (symbol.empty()) {
                continue;
            }
            const std::array<Subscription, 1> one{Subscription{.channel = depth, .inst_id = symbol}};
            std::vector<std::string> frames;
            if (const auto off = state_->protocol.unsubscribe_frame(one); off.has_value()) {
                frames.emplace_back(off->text);
            }
            if (const auto on = state_->protocol.subscribe_frame(one); on.has_value()) {
                frames.emplace_back(on->text);
            }
            if (frames.size() == 2 && pool_.requeue(key_of(depth, symbol), std::move(frames))) {
                RLOG_WARN("okx: {} book broke its seqId chain ({} gaps), resubscribing", symbol,
                          book.gaps());
            }
        }
    }

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the quote channel is
    // ordered here rather than at subscribe time: on a reconnect the order that
    // matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        // THE QUOTE FIRST. Once the sends are paced, whatever goes last
        // effectively never arrives — that is how kucoin's whole depth ladder
        // vanished across 673 instruments — and bbo-tbt is the Bid/Ask column
        // every screen shows.
        const auto quote = channel_for(exec::SubKind::Bbo);
        std::ranges::stable_partition(
            views, [quote](const Subscription& one) { return one.channel == quote; });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(std::span<const Subscription>{views}.subspan(
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
    return std::make_unique<OkxData>(std::move(state));
}

} // namespace raikiri::venues::okx::internal
