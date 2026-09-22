#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::gate::internal {

namespace {

// Gate closes a connection idle for 60 s of application traffic; the app ping
// runs well inside that window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;
constexpr std::uint64_t k_rest_seed_interval_ns = 1'000'000'000ULL;

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send the pool
// refuses is left alone, because the next delta asks again.
void request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        const auto entry = owner->books.find(id.packed);
        if (entry == owner->books.end()) {
            return;
        }
        auto& target = entry->second;
        if (response.transport_failed() || response.status < 200 || response.status >= 300) {
            target.seed_failed();
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_order_book(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        // NOT `book`: the enclosing function takes a DepthBook& by that name,
        // and shadowing it here reads as the ladder rather than the snapshot
        // taken from it.
        if (const auto seeded = target.snapshot(s_to_unix(wire->update_s), response.received);
            seeded.has_value()) {
            owner->sink->on_depth(*seeded);
        }
    };
    if (state.depth_rest.send(
            state.builder.order_book(k_settle_usdt, base.exchange_symbol.view(), k_depth_limit),
            handled)) {
        book.seed_requested();
    }
}

void handle_book_update(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_update(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->contract);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    const auto entry =
        state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    // Asked before the delta is offered, so the frame that triggered the seed
    // is itself buffered behind it.
    if (book.wants_seed()) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    // ASKED BEFORE THE SNAPSHOT IS BUILT, which is the expensive half: a
    // BookSnapshot is 3.2 KB of arrays value-initialised and filled, and the
    // cell it lands in keeps only the latest value. Measured on kucoin,
    // 2026-09-19: the same guard took that gateway from 44.5% to 37.6% of a
    // core.
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

// A whole book off spot.order_book. No seed, no chain, no BookSync: Gate
// republishes the complete depth every interval there, so the only thing that
// can go wrong is a frame that does not parse — and that is silence, not a
// stale book presented as current.
void handle_book_snapshot(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_snapshot(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
        state.sink->on_depth(*book);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.quote_seed_due.erase(bbo->instrument.packed);
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades, state.market);
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
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.funding_seed_due.erase(funding->instrument.packed);
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::BookUpdate:
        handle_book_update(state, frame, received);
        break;
    case EventKind::BookSnapshot:
        handle_book_snapshot(state, frame, received);
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::OrderUpdate:
    case EventKind::UserTrade:
    case EventKind::Pong:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break; // private kinds and control frames are not this socket's
    }
}

// A pool key is one channel on one contract, joined by a byte neither can
// contain. The pool stores keys as opaque strings and hands them back when a
// socket reconnects, so this is the only place that knows the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view contract) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(contract);
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
    return Subscription{.channel = key.substr(0, cut), .contract = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED OR INVALIDATED HERE. Each socket in the pool
    // re-asks for its own keys when it opens; replaying the whole desired set
    // on every one of them is the flood the pool exists to avoid. This handler
    // is shared by every connection and so cannot tell WHICH one reopened —
    // the books belonging to that socket are invalidated by the pool's own
    // open hook, which is handed its keys.
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

class GateData final : public exec::DataClient {
public:
    explicit GateData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{private_host(state_->testnet, state_->market)},
                              .port = std::string{stream_port()},
                              .target = std::string{private_path(state_->testnet, state_->market)}},
                handler_,
                // Gate caps a CONNECTION at 1024 channels; the pool turns that
                // into a per-socket number instead of the process's ceiling.
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) {
                    invalidate_books(keys);
                    due_rest_seeds(keys);
                }} {
        meter_market_data(*state_, state_->depth_rest);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        state_->depth_rest.stop();
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
            // One socket, one tree. Gate spells a spot pair and its perpetual
            // identically, so an id from the other market would be accepted
            // here and answered with the wrong tree's prices.
            return std::unexpected{exec::not_implemented("gate: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = channel_for(kind, state_->market);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto before = state_->desired.size();
        state_->add_subscription(channel, symbol);
        if (state_->desired.size() == before) {
            return {}; // refcounting lives in the gateway core; this is a set
        }
        // THE POOL PLACES IT. A key that does not fit on any existing socket
        // opens another, and only the pool's own ceiling refuses — which is
        // said out loud rather than leaving the instrument silently unwatched.
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "gate: connection pool is full")};
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
            pool_.broadcast(state_->protocol.ping_frame(now.ns / 1'000'000'000ULL));
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        seed_from_rest(now);
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;

    // ONLY THE BOOKS THIS SOCKET FEEDS. A depth book is chained by exactly one
    // connection, so a socket that reopens has broken its own books' delta
    // chains and nobody else's; invalidating every book on any reopen puts the
    // whole catalogue back on the REST seed at once, which is the unpaced
    // burst that earned this IP a ban. The channel is checked as well as the
    // contract: book_ticker for a contract may sit on a different socket from
    // its depth, and only the depth key names a chain that can break.
    void invalidate_books(std::span<const std::string> keys) {
        const auto depth = channel_for(exec::SubKind::Depth, state_->market);
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value() || one->channel != depth) {
                continue;
            }
            const auto* instrument = state_->instruments.find(one->contract);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto entry = state_->books.find(base_of(*instrument).id.packed);
                entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    void due_rest_seeds(std::span<const std::string> keys) {
        const auto quote = channel_for(exec::SubKind::Bbo, state_->market);
        const auto funding = channel_for(exec::SubKind::Funding, state_->market);
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value() || (one->channel != quote && one->channel != funding)) {
                continue;
            }
            if (const auto* instrument = state_->instruments.find(one->contract);
                instrument != nullptr) {
                const auto packed = base_of(*instrument).id.packed;
                (one->channel == quote ? state_->quote_seed_due : state_->funding_seed_due)
                    .insert(packed);
            }
        }
    }

    // One bulk call per kind for every contract still due, at most once a
    // second, so a refusal is asked again rather than dropped.
    void seed_from_rest(UnixNanos now) noexcept {
        if (state_->market != MarketType::Perp || now.ns < next_seed_ns_) {
            return;
        }
        next_seed_ns_ = now.ns + k_rest_seed_interval_ns;
        try {
            if (!state_->quote_seed_due.empty() && !quote_seed_in_flight_) {
                quote_seed_in_flight_ = state_->depth_rest.send(
                    state_->builder.tickers(k_settle_usdt),
                    [this](net::RestResponse&& response) { on_quote_seed(response); });
            }
            if (!state_->funding_seed_due.empty() && !funding_seed_in_flight_) {
                funding_seed_in_flight_ = state_->depth_rest.send(
                    state_->builder.contracts(k_settle_usdt),
                    [this](net::RestResponse&& response) { on_funding_seed(response); });
            }
        } catch (...) {
            return;
        }
    }

    void on_quote_seed(const net::RestResponse& response) {
        quote_seed_in_flight_ = false;
        if (response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<TickerQuoteBuffer>();
        const auto rows = parse_futures_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto bbo = map_bbo(row, state_->instruments, response.received);
            if (bbo.has_value() && state_->quote_seed_due.erase(bbo->instrument.packed) > 0) {
                state_->sink->on_bbo(*bbo);
            }
        }
        state_->quote_seed_due.clear();
    }

    void on_funding_seed(const net::RestResponse& response) {
        funding_seed_in_flight_ = false;
        if (response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<ContractBuffer>();
        const auto rows = parse_contracts(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto funding = map_funding(row, state_->instruments, response.received);
            if (funding.has_value() &&
                state_->funding_seed_due.erase(funding->instrument.packed) > 0) {
                state_->sink->on_funding(*funding);
            }
        }
        state_->funding_seed_due.clear();
    }

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the quote channel is
    // ordered first here rather than at subscribe time: on a reconnect the
    // order that matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        std::ranges::stable_partition(
            views, [](const Subscription& one) { return one.channel.ends_with("book_ticker"); });

        std::vector<std::string> frames;
        const auto now_s = state_->now_s();
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(
                    built, std::min(net::frame_subscriptions(), views.size() - built)),
                now_s);
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    net::WsPool pool_;
    std::uint64_t next_seed_ns_{0};
    bool quote_seed_in_flight_{false};
    bool funding_seed_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<GateData>(std::move(state));
}

} // namespace raikiri::venues::gate::internal
