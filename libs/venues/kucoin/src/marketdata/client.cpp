#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/market_data.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::kucoin::detail {

namespace {

// A day's turnover restated every five minutes. The catalogue endpoint
// answers with every contract, so this is one call however many symbols
// the gateway watches.
constexpr std::uint64_t k_ticker_poll_interval_ns = 300ULL * 1'000'000'000ULL;

// KuCoin allows about ten messages a second on a connection; five leaves room
// for the application ping sharing the same budget.
constexpr std::uint64_t k_subscribe_spacing_ns = 120ULL * 1'000'000ULL;

// Enough sockets for KuCoin's whole catalogue at 1024 keys each: 673 perpetual
// contracts times four topics is 2692, so three would do and six leaves room
// for the contracts KuCoin lists. Far under the fifty connections a user id is
// allowed, which is the ceiling that would actually hurt to trip.
// 673 contracts times four topics is 2692 topic-symbol pairs, and a socket
// carries a hundred — so twenty-seven, and thirty-two leaves room for the
// contracts kucoin lists. Well under the fifty connections one user id is
// allowed, which is the ceiling that would actually hurt.
constexpr std::size_t k_max_connections = 32;

// What one REST pool holds before it refuses on its own: two warm connections
// times eight waiters. It matters here because RestClient asks the admit gate
// BEFORE the pool answers, so a request the pool was always going to drop
// still costs one of the 2000 KuCoin allows in 30 s — and a cold book asks
// again on every level2 frame. Stopping short of that ceiling also leaves the
// pool room for the bullet mint, the one call market data cannot go without.
constexpr std::size_t k_rest_queue_headroom = 8;

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises.
//
// THE BOOK IS MARKED IN FLIGHT ONLY IF THE SEND WAS TAKEN. The rate budget
// refuses a seed as readily as a busy pool does, and a book marked before the
// refusal waits forever for a request nobody made; left cold, it still wants a
// seed and the next change frame asks again.
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
        const auto wire = parse_level2_snapshot(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        if (const auto book = target.snapshot(UnixNanos{wire->ts_ns}, response.received);
            book.has_value()) {
            owner->sink->on_depth(*book);
        }
    };
    if (state.depth_rest.send(state.builder.level2_snapshot(base.exchange_symbol.view()),
                              handled)) {
        book.seed_requested();
    }
}

// A whole book off /spotMarket/level2Depth50. No seed, no chain: KuCoin
// republishes the complete depth there, so the only failure available is a
// frame that does not parse — silence, never a stale book served as current.
void handle_book_snapshot(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_snapshot(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    if (const auto depth = map_book(*wire, state.instruments, received); depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_book_change(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto wire = parse_book_change(frame);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->symbol);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    // Asked before the change is offered, so the frame that triggered the seed
    // is itself buffered behind it.
    if (book.wants_seed()) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::Bbo: {
        const auto wire = parse_bbo(frame, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Trade: {
        const auto wire = parse_trade(frame, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::FundingRate: {
        const auto wire = parse_funding_rate(frame);
        if (!wire.has_value()) {
            return;
        }
        auto* memo = state.memo_for(wire->symbol);
        if (memo == nullptr) {
            break; // nothing to accumulate into, and nothing to publish
        }
        if (const auto funding =
                map_funding(*wire, wire->symbol, state.instruments, *memo, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Depth:
        if (state.market == MarketType::Spot) {
            handle_book_snapshot(state, frame, received);
        } else {
            handle_book_change(state, frame, received);
        }
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            if (is_expired_token(err->code)) {
                state.schedule_reopen();
            } else {
                ++state.subscribe_refusals;
            }
        }
        break;
    }
    case EventKind::MarkPrice: {
        const auto wire = parse_mark_price(frame);
        if (!wire.has_value()) {
            return;
        }
        auto* memo = state.memo_for(wire->symbol);
        if (memo == nullptr) {
            break;
        }
        if (const auto funding = map_mark_price(*wire, state.instruments, *memo, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::OrderChange: // private topic; never reaches this socket
    case EventKind::Welcome:
    case EventKind::Ack:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

// A pool key is one topic on one symbol, joined by a byte neither can contain.
// The pool stores keys as opaque strings and hands them back when a socket
// reconnects, so this is the only place that knows the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view topic, std::string_view symbol) {
    std::string key{topic};
    key.push_back(k_key_sep);
    key.append(symbol);
    return key;
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE, AND NOTHING IS INVALIDATED HERE. Each
    // socket in the pool re-asks for its own keys when it opens; replaying the
    // whole desired set on every one of them is the flood the pool exists to
    // avoid. The pool forwards this per connection, so a blanket invalidation
    // would send all ~673 futures books back for a REST seed every time any
    // one of six sockets blinked — the pool's OnConnectionOpen invalidates the
    // books that socket actually carries instead.
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason reason) override {
        if (reason != net::WsCloseReason::Requested) {
            state_->schedule_reopen();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
};

class KucoinData final : public exec::DataClient {
public:
    explicit KucoinData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->market)}, .port = "443"}} {
        // BOTH POOLS, because KuCoin meters one allowance per IP and both
        // spend it: the seeds and the catalogue poll ride depth_rest, the
        // bullet token and the funding seed ride this one. Every caller below
        // treats a refusal as a retry on its next tick, never as a failure.
        //
        // The pool's own room is asked FIRST and costs nothing; the budget is
        // only spent on a request that has somewhere to go.
        const auto gate = [owner = state_.get()](const net::RestClient& client) {
            return client.in_flight() < k_rest_queue_headroom && owner->admit_poll();
        };
        state_->depth_rest.set_admit(
            [gate, client = &state_->depth_rest](std::string_view) { return gate(*client); });
        rest_.set_admit([gate, client = &rest_](std::string_view) { return gate(*client); });
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        state_->depth_rest.start();
        started_ = true;
        return open_stream();
    }

    void stop() override {
        // Dropped, not waited for: the callback co-owns the cell, so an answer
        // after this writes into something only it still points at.
        bullet_.reset();
        if (pool_) {
            pool_->stop();
            pool_.reset();
        }
        state_->reopen_due = false;
        rest_.stop();
        state_->depth_rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return pool_ != nullptr && pool_->connected();
    }

    // What this venue's sockets were told no about. Counted by the
    // adapter since it was written; this is what finally reads it.
    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override {
        return state_->subscribe_refusals;
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // The bullet this socket connected with belongs to ONE tree, and
            // the two trees share neither topics nor payload shapes.
            return std::unexpected{exec::not_implemented("kucoin: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto topic = topic_for(kind, state_->market);
        if (topic.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto before = state_->desired.size();
        state_->add_subscription(topic, symbol);
        // Funding and the mark share /contract/instrument, so the second of the
        // two adds no key — and must still reach the seed below.
        if (state_->desired.size() != before && pool_ != nullptr) {
            // THE POOL PLACES IT. A key that fits on no existing socket opens
            // another, and only the pool's own ceiling refuses — which is said
            // out loud rather than leaving the instrument silently unwatched.
            const std::array<std::string, 1> one{key_of(topic, symbol)};
            if (!pool_->subscribe(one)) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, "kucoin: connection pool is full")};
            }
        }
        if (kind == exec::SubKind::Funding) {
            if (!seed_funding(symbol)) {
                // Refused, not lost. /contract/instrument states a funding rate
                // only when KuCoin restates one, so a dropped seed is a blank
                // column until the next eight-hour settlement.
                pending_funding_seeds_.emplace_back(symbol);
            }
            wanted_.push_back(instrument);
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        if (state_->reopen_due && state_->clock->mono().ns >= state_->reopen_at_ns) {
            state_->reopen_due = false;
            reopen();
        }
        collect_reopen(now);
        state_->io.poll();
        if (pool_) {
            pool_->poll(now);
        }
        if (connected() && now.ns >= state_->next_app_ping_ns) {
            if (const auto frame = state_->protocol.ping_frame(); !frame.empty()) {
                // EVERY socket. KuCoin closes a connection whose application
                // ping stops arriving, so pinging one of six kills the other
                // five a ping interval later.
                pool_->broadcast(frame);
            }
            state_->next_app_ping_ns = now.ns + state_->app_ping_interval_ns;
        }
        for (auto* pumped : state_->pumps) {
            pumped->pump(now);
        }
        drain_funding_seeds();
        poll_ticker(now);
    }

    // The 24-hour window comes off the CATALOGUE endpoint, which is the only
    // place kucoin states it — no stream this adapter subscribes to carries a
    // turnover. Slow, because the figure it reads covers a day.
    void poll_ticker(UnixNanos now) noexcept {
        if (wanted_.empty() || now.ns < next_ticker_poll_ns_ || ticker_in_flight_) {
            return;
        }
        ticker_in_flight_ = true;
        try {
            if (!state_->depth_rest.send(
                    state_->builder.contracts_active(),
                    [this](net::RestResponse&& response) { on_ticker(response); })) {
                ticker_in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            ticker_in_flight_ = false;
            return;
        }
        // Only a call the venue was actually asked buys the next five minutes.
        next_ticker_poll_ns_ = now.ns + k_ticker_poll_interval_ns;
    }

    // Funding seeds the budget refused, one a tick. A loop here would ask for
    // the same refusal once per pending symbol.
    void drain_funding_seeds() noexcept {
        if (pending_funding_seeds_.empty()) {
            return;
        }
        try {
            if (!seed_funding(pending_funding_seeds_.back())) {
                return; // still no room; the next tick asks again
            }
        } catch (...) {
            return;
        }
        pending_funding_seeds_.pop_back();
    }

    void on_ticker(const net::RestResponse& response) noexcept {
        ticker_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local ContractBuffer storage;
        const auto rows = parse_contracts(response.body, storage);
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
            if (std::ranges::none_of(wanted_, [id](InstrumentId sub) { return sub == id; })) {
                continue;
            }
            if (const auto stats = map_ticker_stats(row, state_->instruments, received);
                stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

private:
    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the ordering lives
    // here rather than at subscribe time: on a reconnect the order that
    // matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            const auto cut = key.find(k_key_sep);
            if (cut == std::string::npos) {
                continue;
            }
            views.push_back({.topic = std::string_view{key}.substr(0, cut),
                             .symbol = std::string_view{key}.substr(cut + 1)});
        }
        // GROUPED BY TOPIC so a frame batches, but ordered by what a screen
        // needs FIRST. Sorted lexicographically, tickerV2 — the top of book —
        // came last of the four, and once the sends are paced that meant the
        // one column everybody looks at was the last to arrive and, on a large
        // catalogue, did not arrive at all.
        const auto rank = [](std::string_view topic) {
            if (topic.ends_with("tickerV2") || topic.ends_with("/market/ticker")) {
                return 0; // the quote
            }
            if (topic.ends_with("instrument")) {
                return 1; // funding and the mark
            }
            if (topic.ends_with("execution") || topic.ends_with("match")) {
                return 2; // prints
            }
            return 3; // the ladder, which only the open market draws
        };
        std::ranges::sort(views, [&rank](const Subscription& a, const Subscription& b) {
            const auto ra = rank(a.topic);
            const auto rb = rank(b.topic);
            return ra != rb ? ra < rb : a.topic < b.topic;
        });

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

    // The books ONE reopened socket broke, and no others. A depth book is fed
    // by a single connection, so its delta chain survives every reconnect but
    // its own — and each futures book that goes cold reseeds over REST against
    // /api/v1/level2/snapshot, which is the burst this venue cannot afford
    // ~673 of. Spot subscribes the level2Depth50 SNAPSHOT topic, which
    // republishes whole books and keeps none here, so the loop finds nothing.
    void invalidate_books(std::span<const std::string> keys) noexcept {
        const auto depth_topic = topic_for(exec::SubKind::Depth, state_->market);
        for (const auto& key : keys) {
            const auto cut = key.find(k_key_sep);
            if (cut == std::string::npos || std::string_view{key}.substr(0, cut) != depth_topic) {
                continue; // only level2 names a book
            }
            const auto* listed = state_->instruments.find(std::string_view{key}.substr(cut + 1));
            if (listed == nullptr) {
                continue;
            }
            if (const auto entry = state_->books.find(base_of(*listed).id.packed);
                entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    void reopen() noexcept {
        try {
            begin_reopen();
        } catch (...) {
            state_->schedule_reopen();
        }
    }

    // BOOT ONLY. Blocking here is the bootstrap case asio documents: nothing is
    // running yet, and a gateway that streams before it has a socket is worse
    // than one that takes a second to start. A reopen at RUNTIME must not come
    // through here — see begin_reopen.
    std::expected<void, exec::VenueError> open_stream() {
        const auto body =
            usable_body(await_response(*state_, rest_, state_->builder.bullet_public(), 10'000));
        if (!body.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{body.error()};
        }
        return open_stream_with(*body);
    }

    // A RUNTIME REOPEN MUST NOT STOP THE GATEWAY LOOP. This ran inside poll()
    // and blocked it for up to ten seconds while KuCoin minted a bullet token —
    // ten seconds in which no book moved and no cancel could leave, on every
    // reconnect. It was the only call in the tree that broke the reactor's own
    // promise that no venue's silence can delay a cancel (io_context.hpp).
    //
    // So the mint is asked for here and collected on a later pass. The landing
    // is a cell the callback co-owns: giving up on the deadline below is not
    // cancelling, and the pool still holds the callback.
    void begin_reopen() noexcept {
        if (bullet_ != nullptr) {
            return; // a mint is already in flight; its answer reopens us
        }
        auto landing = std::make_shared<std::optional<net::RestResponse>>();
        if (!rest_.send(state_->builder.bullet_public(),
                        [landing](net::RestResponse&& response) {
                            *landing = std::move(response);
                        })) {
            state_->schedule_reopen();
            return;
        }
        bullet_ = std::move(landing);
        bullet_due_ns_ = state_->clock->mono().ns + k_bullet_deadline_ns;
    }

    // OWN DEADLINE, not the transport's. RestClient ages its queue and bounds
    // its name lookup, so an answer does arrive — but a venue left waiting on a
    // mint that never lands is a venue that never reconnects, and that must not
    // depend on a guarantee made in another library.
    void collect_reopen(UnixNanos now) noexcept {
        if (bullet_ == nullptr) {
            return;
        }
        if (!bullet_->has_value()) {
            if (now.ns >= bullet_due_ns_) {
                bullet_.reset();
                state_->schedule_reopen();
            }
            return;
        }
        const auto answer = std::move(*bullet_);
        bullet_.reset();
        const auto body = usable_body(answer);
        if (!body.has_value()) {
            state_->schedule_reopen();
            return;
        }
        (void)open_stream_with(*body).has_value(); // failures schedule their own retry
    }

    std::expected<void, exec::VenueError> open_stream_with(const std::string& body) {
        const auto bullet = parse_bullet(body);
        if (!bullet.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bullet.error()))};
        }
        const auto endpoint =
            make_endpoint(bullet->endpoint, bullet->token, state_->next_connect_id());
        if (!endpoint.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(endpoint.error()))};
        }
        if (bullet->ping_interval_ms != 0) {
            state_->app_ping_interval_ns = bullet->ping_interval_ms * 1'000'000ULL;
        }
        state_->next_app_ping_ns = 0;

        if (pool_) {
            pool_->stop();
        }
        // ONE BULLET FOR THE WHOLE POOL, and the token and connect id sit in
        // the target every socket is built from. KuCoin wants a connect id per
        // connection, so the sockets here present the same one; giving each its
        // own needs a per-connection config on net::WsPool, which this venue
        // cannot add on its own. What keeps it honest meanwhile: the pool opens
        // its sockets within a second of the mint, and a token the venue
        // refuses answers 401, which schedules the reopen that mints again.
        pool_ = std::make_unique<net::WsPool>(
            state_->io,
            net::WsConfig{
                .host = endpoint->host, .port = endpoint->port, .target = endpoint->target},
            handler_,
            // ONE HUNDRED, WHICH IS KUCOIN'S OWN CEILING PER CONNECTION, and
            // it is counted in topic-symbol pairs however they are batched
            // into frames. Set to k_max_subscriptions_per_connection (1024)
            // this looked fine and was not: the venue took the first hundred
            // on each socket and dropped the rest in silence, so with the
            // quote ranked first every symbol quoted while the ladder — ranked
            // last — never arrived at all. Measured: 10 instruments gave
            // depth=13866, 673 gave depth=0.
            net::WsPoolConfig{.max_keys_per_connection = k_max_topics_per_connection,
                              .send_spacing_ns = k_subscribe_spacing_ns,
                              .max_connections = k_max_connections},
            [this](std::span<const std::string> keys) { return build_frames(keys); },
            [this](std::span<const std::string> keys) { invalidate_books(keys); });
        pool_->start();

        // The desired set outlives the pool the bullet expiry throws away, so
        // it is placed again here — including whatever subscribe() recorded
        // while there was no pool to place it on.
        std::vector<std::string> keys;
        keys.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            keys.push_back(key_of(entry.topic, entry.symbol));
        }
        if (!keys.empty() && !pool_->subscribe(keys)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin: connection pool is full")};
        }
        // The handler's own on_open is NOT called here. The pool forwards it
        // per connection, and hands the same open to invalidate_books with
        // that connection's keys; calling it here as well would fire the
        // connectivity event once per stream on top.
        return {};
    }

    [[nodiscard]] bool seed_funding(std::string_view symbol) {
        std::string owned{symbol};
        return rest_.send(
            state_->builder.funding_rate_current(symbol),
            [this, owned = std::move(owned)](net::RestResponse&& response) {
                const auto body =
                    usable_body(std::optional<net::RestResponse>{std::move(response)});
                if (!body.has_value()) {
                    return; // a contract without funding answers 415000
                }
                const auto wire = parse_funding_current(*body);
                if (!wire.has_value()) {
                    return;
                }
                const auto now = state_->clock->wall();
                auto* memo = state_->memo_for(owned);
                if (memo == nullptr) {
                    return;
                }
                if (const auto funding = map_funding(*wire, owned, state_->instruments, *memo, now);
                    funding.has_value()) {
                    state_->sink->on_funding(*funding);
                }
            });
    }

    std::shared_ptr<VenueState> state_;
    // The mint a runtime reopen is waiting on, and when to give up on it.
    std::shared_ptr<std::optional<net::RestResponse>> bullet_;
    std::uint64_t bullet_due_ns_{0};
    PublicHandler handler_;
    net::RestClient rest_;
    std::vector<InstrumentId> wanted_;
    std::vector<std::string> pending_funding_seeds_;
    std::uint64_t next_ticker_poll_ns_{0};
    bool ticker_in_flight_{false};
    std::unique_ptr<net::WsPool> pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<KucoinData>(std::move(state));
}

} // namespace raikiri::venues::kucoin::detail
