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
constexpr std::size_t k_max_connections = 6;

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send the pool
// refuses is left alone, because the next change asks again.
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

    // NOTHING IS RESUBSCRIBED HERE. Each socket in the pool re-asks for its
    // own keys when it opens; replaying the whole desired set on every one of
    // them is the flood the pool exists to avoid. The pool does not forward a
    // connection's open either, so the caller invokes this once per stream —
    // that is, once per bullet, since the token lives in the URL.
    void on_open() override {
        // The sequence the old book stopped at says nothing about where this
        // subscription resumes, so every book goes back to wanting a seed.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
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
                net::RestConfig{.host = std::string{rest_host(state_->market)}, .port = "443"}} {}

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        state_->depth_rest.start();
        started_ = true;
        return open_stream();
    }

    void stop() override {
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
            seed_funding(symbol);
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
        poll_ticker(now);
    }

    // The 24-hour window comes off the CATALOGUE endpoint, which is the only
    // place kucoin states it — no stream this adapter subscribes to carries a
    // turnover. Slow, because the figure it reads covers a day.
    void poll_ticker(UnixNanos now) noexcept {
        if (wanted_.empty() || now.ns < next_ticker_poll_ns_ || ticker_in_flight_) {
            return;
        }
        next_ticker_poll_ns_ = now.ns + k_ticker_poll_interval_ns;
        ticker_in_flight_ = true;
        try {
            if (!state_->depth_rest.send(
                    state_->builder.contracts_active(),
                    [this](net::RestResponse&& response) { on_ticker(response); })) {
                ticker_in_flight_ = false;
            }
        } catch (...) {
            ticker_in_flight_ = false;
        }
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

    void reopen() noexcept {
        try {
            (void)open_stream().has_value(); // failures schedule their own retry
        } catch (...) {
            state_->schedule_reopen();
        }
    }

    std::expected<void, exec::VenueError> open_stream() {
        const auto body =
            usable_body(await_response(*state_, rest_, state_->builder.bullet_public(), 10'000));
        if (!body.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{body.error()};
        }
        const auto bullet = parse_bullet(*body);
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
            // KuCoin's own ceiling is 100 TOPICS, and batching keeps that at
            // one per kind — so this bounds our bookkeeping per socket.
            net::WsPoolConfig{.max_keys_per_connection = k_max_subscriptions_per_connection,
                              .send_spacing_ns = k_subscribe_spacing_ns,
                              .max_connections = k_max_connections},
            [this](std::span<const std::string> keys) { return build_frames(keys); });
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
        // per connection, which is what invalidates the books whose delta
        // chain that connection's reconnect has just broken; calling it here
        // as well would fire the connectivity event once per stream on top.
        return {};
    }

    void seed_funding(std::string_view symbol) {
        std::string owned{symbol};
        (void)rest_.send(state_->builder.funding_rate_current(symbol),
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
                             if (const auto funding =
                                     map_funding(*wire, owned, state_->instruments, *memo, now);
                                 funding.has_value()) {
                                 state_->sink->on_funding(*funding);
                             }
                         });
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::RestClient rest_;
    std::vector<InstrumentId> wanted_;
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
