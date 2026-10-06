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

namespace raikiri::venues::kucoin::internal {

namespace {

// the catalogue call covers every contract at once
constexpr std::uint64_t k_ticker_poll_interval_ns = 300ULL * 1'000'000'000ULL;
// on the uta feed this poll is the funding source, and on spot the only 24h window
constexpr std::uint64_t k_uta_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;

// the rest client asks the rate gate before the pool, so a request the pool drops still spends budget
constexpr std::size_t k_rest_queue_headroom = 8;

// mark the book in flight only if the send was taken
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
            owner->hold_after_refusal(response);
            target.seed_failed(response.received.ns);
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_level2_snapshot(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed(response.received.ns);
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        if (const auto seeded = target.snapshot(UnixNanos{wire->ts_ns}, response.received); seeded.has_value()) {
            owner->sink->on_depth(*seeded);
        }
    };
    if (state.depth_rest.send(state.builder.level2_snapshot(base.exchange_symbol.view()), handled)) {
        book.seed_requested();
    }
}

// spot republishes the whole book, so there is no seed or chain
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
    // asked before the change is offered, so the triggering frame is buffered behind the seed
    if (book.wants_seed(received.ns)) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    const auto ts_event = ms_to_unix(wire->ts_ms, UnixNanos{});
    // ask the sink before building the snapshot; the build is the expensive part
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

// the applied book's touch is the quote, so one frame yields both
void handle_uta_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local const auto storage = std::make_unique<LevelBuffer>();
    const auto wire = parse_uta_book(frame, *storage);
    if (!wire.has_value()) {
        if (const auto print = parse_uta_trade(frame); print.has_value()) {
            if (const auto trade = map_trade(*print, state.instruments, received); trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        return; // acknowledgements, pongs and anything else are neither
    }
    const auto* instrument = state.instruments.find(wire->symbol);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.uta_book_for(base);
    if (book.apply(*wire) != BookResult::Applied) {
        return; // waiting, replayed, gapped or unconvertible: silence, never a guess
    }
    const UnixNanos ts_event{wire->ts_ns};
    if (const auto top = book.bbo(ts_event, received); top.has_value()) {
        state.sink->on_bbo(*top);
    }
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    if (state.uta) {
        handle_uta_frame(state, frame, received);
        return;
    }
    const auto event = parse_event(frame);
    if (!event.has_value()) {
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
        if (const auto funding = map_funding(*wire, wire->symbol, state.instruments, *memo, received);
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
        if (const auto funding = map_mark_price(*wire, state.instruments, *memo, received); funding.has_value()) {
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

// a pool key is topic and symbol joined by a byte neither contains
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

    // each pooled socket resubscribes its own keys; OnConnectionOpen invalidates only that socket's books
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
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host(state_->market)}, .port = "443"}} {
        // both pools spend kucoin's per-ip allowance; the pool's room is checked before the budget
        const auto gate = [owner = state_.get()](const net::RestClient& client, std::string_view target) {
            return client.in_flight() < k_rest_queue_headroom && owner->admit_poll(target);
        };
        state_->depth_rest.set_admit(
            [gate, client = &state_->depth_rest](std::string_view target) { return gate(*client, target); });
        rest_.set_admit([gate, client = &rest_](std::string_view target) { return gate(*client, target); });
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        state_->depth_rest.start();
        started_ = true;
        return open_stream();
    }

    void stop() override {
        // the callback co-owns the cell, so it is safe to drop
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

    [[nodiscard]] bool connected() const noexcept override { return pool_ != nullptr && pool_->connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // a bullet serves one market tree
            return std::unexpected{exec::not_implemented("kucoin: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        // on uta one channel serves the quote and the ladder
        const auto topic = state_->uta ? uta_topic_for(kind) : topic_for(kind, state_->market);
        if (topic.empty()) {
            // uta v2 has no funding topic; funding comes off the catalogue poll, so list the instrument
            if (state_->uta && kind == exec::SubKind::Funding) {
                wanted_.push_back(instrument);
                return {};
            }
            return DataClient::subscribe(kind, instrument);
        }
        const auto before = state_->desired.size();
        state_->add_subscription(topic, symbol);
        // funding and mark share /contract/instrument, so the second adds no key but still needs the seed
        if (state_->desired.size() != before && pool_ != nullptr) {
            const std::array<std::string, 1> one{key_of(topic, symbol)};
            if (!pool_->subscribe(one)) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, "kucoin: connection pool is full")};
            }
        }
        if (kind == exec::SubKind::Funding) {
            if (!seed_funding(symbol)) {
                // a dropped funding seed leaves the column blank until the next settlement
                pending_funding_seeds_.emplace_back(symbol);
            }
            wanted_.push_back(instrument);
        } else if (kind == exec::SubKind::Bbo && state_->market == MarketType::Spot) {
            wanted_.push_back(instrument); // spot has no funding to put it on the list
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
                // kucoin closes a connection whose application ping stops, so ping every socket
                pool_->broadcast(frame);
            }
            state_->next_app_ping_ns = now.ns + state_->app_ping_interval_ns;
        }
        for (auto* pumped : state_->pumps) {
            pumped->pump(now);
        }
        drain_funding_seeds();
        reseed_quiet_books(now);
        poll_ticker(now);
    }

    // otherwise a refused seed is retried only on the book's next change frame
    void reseed_quiet_books(UnixNanos now) noexcept {
        if (now.ns < next_reseed_sweep_ns_) {
            return;
        }
        next_reseed_sweep_ns_ = now.ns + DepthBook::k_seed_retry_ns;
        for (auto& [packed, book] : state_->books) {
            if (!book.wants_seed(now.ns)) {
                continue;
            }
            if (const auto* instrument = state_->instruments.find(InstrumentId{packed}); instrument != nullptr) {
                request_seed(*state_, book, base_of(*instrument));
            }
        }
    }

    // 24h window comes from the catalogue on futures and allTickers on spot
    void poll_ticker(UnixNanos now) noexcept {
        if (wanted_.empty() || now.ns < next_ticker_poll_ns_ || ticker_in_flight_) {
            return;
        }
        ticker_in_flight_ = true;
        const bool spot = state_->market == MarketType::Spot;
        // depth_rest is pinned to the futures host; rest_ follows the market.
        auto& client = spot ? rest_ : state_->depth_rest;
        try {
            if (!client.send(spot ? state_->builder.spot_all_tickers() : state_->builder.contracts_active(),
                             [this, spot](net::RestResponse&& response) {
                                 spot ? on_spot_ticker(response) : on_ticker(response);
                             })) {
                ticker_in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            ticker_in_flight_ = false;
            return;
        }
        // only a sent call advances the interval
        next_ticker_poll_ns_ = now.ns +
                               (state_->uta || spot ? k_uta_funding_poll_interval_ns : k_ticker_poll_interval_ns);
    }

    // one refused funding seed per tick
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

    void on_spot_ticker(const net::RestResponse& response) noexcept {
        ticker_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local SpotTickerBuffer storage;
        const auto rows = parse_spot_tickers(response.body, storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto stats = map_ticker_stats(row, state_->instruments, received);
            if (stats.has_value() &&
                std::ranges::any_of(wanted_, [id = stats->instrument](InstrumentId sub) { return sub == id; })) {
                state_->sink->on_ticker_stats(*stats);
            }
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
            if (const auto stats = map_ticker_stats(row, state_->instruments, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
            // uta only: the classic feed has its own funding stream writing the same cell
            if (state_->uta && !row.funding_fee_rate.empty()) {
                publish_row_funding(row, received);
            }
        }
    }

    void publish_row_funding(const WireContract& row, UnixNanos received) noexcept {
        auto* memo = state_->memo_for(row.symbol);
        if (memo == nullptr) {
            return;
        }
        WireFundingRate wire{};
        wire.symbol = row.symbol;
        wire.rate = row.funding_fee_rate;
        wire.predicted_rate = row.predicted_funding_fee_rate;
        wire.granularity_ms = row.funding_granularity_ms;
        wire.next_funding_ms = row.next_funding_ms;
        wire.ts_ms = received.ns / 1'000'000ULL;
        if (const auto funding = map_funding(wire, row.symbol, state_->instruments, *memo, received);
            funding.has_value()) {
            state_->sink->on_funding(*funding);
        }
    }

private:
    // called on every open of the socket
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        if (state_->uta) {
            // one frame per symbol; obu refuses symbols arrays
            std::vector<std::string> out;
            out.reserve(keys.size());
            for (const auto& key : keys) {
                const auto cut = key.find(k_key_sep);
                if (cut == std::string::npos) {
                    continue;
                }
                const std::array<Subscription, 1> one{Subscription{.topic = std::string_view{key}.substr(0, cut),
                                                                   .symbol = std::string_view{key}.substr(cut + 1)}};
                if (const auto frame = state_->protocol.uta_subscribe_frame(one); frame.has_value()) {
                    out.emplace_back(frame->text);
                }
            }
            return out;
        }
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            const auto cut = key.find(k_key_sep);
            if (cut == std::string::npos) {
                continue;
            }
            views.push_back(
                {.topic = std::string_view{key}.substr(0, cut), .symbol = std::string_view{key}.substr(cut + 1)});
        }
        // grouped by topic, quote first so it lands before the slower kinds
        const auto rank = [](std::string_view topic) {
            if (topic.ends_with("tickerV2") || topic.ends_with("level1")) {
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
            const auto frame = state_->protocol.subscribe_frame(std::span<const Subscription>{views}.subspan(built));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    // invalidate only the books this reopened socket fed; each cold futures book reseeds over rest
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
            if (const auto entry = state_->books.find(base_of(*listed).id.packed); entry != state_->books.end()) {
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

    // boot only, blocking; runtime reopens go through begin_reopen
    std::expected<void, exec::VenueError> open_stream() {
        if (state_->uta) {
            // this host needs no bullet
            return open_stream_at(
                StreamEndpoint{.host = std::string{k_uta_futures_host}, .port = "443", .target = "/"});
        }
        const auto body = usable_body(await_response(*state_, rest_, state_->builder.bullet_public(), 10'000));
        if (!body.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{body.error()};
        }
        return open_stream_with(*body);
    }

    // a runtime reopen must not block the gateway loop: request the mint here, collect it later
    void begin_reopen() noexcept {
        if (state_->uta) {
            // no mint, so the reopen is immediate
            (void)open_stream_at(StreamEndpoint{.host = std::string{k_uta_futures_host}, .port = "443", .target = "/"})
                .has_value();
            return;
        }
        if (bullet_ != nullptr) {
            return; // a mint is already in flight; its answer reopens us
        }
        auto landing = std::make_shared<std::optional<net::RestResponse>>();
        if (!rest_.send(state_->builder.bullet_public(),
                        [landing](net::RestResponse&& response) { *landing = std::move(response); })) {
            state_->schedule_reopen();
            return;
        }
        bullet_ = std::move(landing);
        bullet_due_ns_ = state_->clock->mono().ns + k_bullet_deadline_ns;
    }

    // own deadline so a lost mint cannot stall reconnection
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bullet.error()))};
        }
        const auto endpoint = make_endpoint(bullet->endpoint, bullet->token, state_->next_connect_id());
        if (!endpoint.has_value()) {
            state_->schedule_reopen();
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(endpoint.error()))};
        }
        if (bullet->ping_interval_ms != 0) {
            state_->app_ping_interval_ns = bullet->ping_interval_ms * 1'000'000ULL;
        }
        return open_stream_at(*endpoint);
    }

    std::expected<void, exec::VenueError> open_stream_at(const StreamEndpoint& endpoint) {
        state_->next_app_ping_ns = 0;

        if (pool_) {
            pool_->stop();
        }
        // one bullet and connect id for the whole pool; a refused token answers 401 and remints
        pool_ = std::make_unique<net::WsPool>(
            state_->io,
            net::WsConfig{.host = endpoint.host, .port = endpoint.port, .target = endpoint.target},
            handler_,
            // kucoin accepts 100 topic-symbol pairs per connection and drops the rest silently
            net::WsPoolConfig{},
            [this](std::span<const std::string> keys) { return build_frames(keys); },
            [this](std::span<const std::string> keys) { invalidate_books(keys); });
        pool_->start();

        // the desired set outlives the pool, so it is placed again here
        std::vector<std::string> keys;
        keys.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            keys.push_back(key_of(entry.topic, entry.symbol));
        }
        if (!keys.empty() && !pool_->subscribe(keys)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "kucoin: connection pool is full")};
        }
        // the pool forwards on_open per connection; calling it here would double the event
        return {};
    }

    [[nodiscard]] bool seed_funding(std::string_view symbol) {
        std::string owned{symbol};
        return rest_.send(state_->builder.funding_rate_current(symbol),
                          [this, owned = std::move(owned)](net::RestResponse&& response) {
                              if (response.status == 429) {
                                  state_->hold_after_refusal(response);
                                  pending_funding_seeds_.push_back(owned);
                                  return;
                              }
                              const auto body = usable_body(std::optional<net::RestResponse>{std::move(response)});
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
    // the mint a runtime reopen is waiting on, and when to give up on it
    std::shared_ptr<std::optional<net::RestResponse>> bullet_;
    std::uint64_t bullet_due_ns_{0};
    PublicHandler handler_;
    net::RestClient rest_;
    std::vector<InstrumentId> wanted_;
    std::vector<std::string> pending_funding_seeds_;
    std::uint64_t next_ticker_poll_ns_{0};
    std::uint64_t next_reseed_sweep_ns_{0};
    bool ticker_in_flight_{false};
    std::unique_ptr<net::WsPool> pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<KucoinData>(std::move(state));
}

} // namespace raikiri::venues::kucoin::internal
