#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/xt/book.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/market_data.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/ws.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::xt::internal {

namespace {

// xt has no funding channel; one symbol is polled per tick and the whole set each period
constexpr std::uint64_t k_funding_period_ns = 10'000'000'000ULL;

constexpr std::uint64_t k_funding_min_spacing_ns = 250'000'000ULL;

// the socket is the source of marks and indices; two bulk rest calls check it on every open and at this pace
constexpr std::uint64_t k_mark_reconcile_interval_ns = 60'000'000'000ULL;

// inside xt's 30 s idle window with room for a miss and a reconnect
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
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
    case EventKind::Error: {
        if (const auto err = parse_stream_ack(frame); err.has_value()) {
            state.last_error.assign(err->msg);
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame, state.market);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received); stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::DepthUpdate:
        // not subscribed; named so a stray frame is recognised
        break;
    case EventKind::MarkPrice: {
        const auto wire = parse_mark_price(frame);
        if (!wire.has_value()) {
            return;
        }
        auto* memo = state.memo_for(wire->symbol);
        if (memo == nullptr) {
            break; // nothing to accumulate into, and nothing to publish
        }
        if (const auto funding = map_mark_price(*wire, state.instruments, *memo, received); funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::IndexPrice: {
        const auto wire = parse_mark_price(frame);
        if (!wire.has_value()) {
            return;
        }
        auto* memo = state.memo_for(wire->symbol);
        if (memo == nullptr) {
            break;
        }
        if (const auto funding = map_index_price(*wire, state.instruments, *memo, received); funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Ack:
    case EventKind::Unknown:
        break;
    }
}

// pool key is channel and symbol joined by a byte neither contains; params come from params_for
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view symbol) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(symbol);
    return key;
}

// by channel so a frame rebuilt on reconnect carries the same params
[[nodiscard]] std::string_view params_for(std::string_view channel, MarketType market) noexcept {
    const auto depth = topic_for(exec::SubKind::Bbo, market);
    return channel == depth.channel ? depth.params : std::string_view{};
}

// inverse of key_of; views into the pool-owned key
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key, MarketType market) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    const auto channel = key.substr(0, cut);
    return Subscription{.channel = channel, .symbol = key.substr(cut + 1), .params = params_for(channel, market)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // each socket re-asks for its own keys; depth snapshots are self-contained, so nothing needs invalidating
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

class XtData final : public exec::DataClient {
public:
    explicit XtData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->market)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path(state_->market)}},
                handler_,
                // the ceiling is per connection
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
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

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // spot pairs and perpetuals share symbols, so the other market's id is refused
            return std::unexpected{exec::not_implemented("xt: this pool serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            if (state_->market != MarketType::Perp) {
                // spot pays no funding; the poll would answer with the perpetual's rate
                return std::unexpected{exec::not_implemented("xt: spot pays no funding")};
            }
            // the mark has its own channel and FundingUpdate carries it
            if (const auto placed = want(topic_for(exec::SubKind::MarkPrice, state_->market), symbol);
                !placed.has_value()) {
                return placed;
            }
            if (const auto placed = want(index_topic(), symbol); !placed.has_value()) {
                return placed;
            }
            return watch_funding(symbol);
        }
        const auto topic = topic_for(kind, state_->market);
        if (topic.channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (const auto placed = want(topic, symbol); !placed.has_value()) {
            return placed;
        }
        if (kind == exec::SubKind::Bbo) {
            // rides with the bbo since xt streams no funding
            return want(ticker_topic(), symbol);
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
        if (now.ns >= state_->next_app_ping_ns) {
            // every socket: the idle timer is per connection and counts only client sends
            pool_.broadcast(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        poll_funding(now);
        reconcile_marks(now);
        // private sockets share this timer since ExecutionClient has no poll hook
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    // records the topic and hands the key to the pool; only the pool's ceiling refuses
    std::expected<void, exec::VenueError> want(const Topic& topic, std::string_view symbol) {
        if (!state_->add_subscription(topic.channel, symbol)) {
            return {}; // already asked for; refcounting lives in the gateway core
        }
        const std::array<std::string, 1> one{key_of(topic.channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "xt: connection pool is full")};
        }
        return {};
    }

    // called on every open of the socket; send order matters under pacing
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        marks_due_ = true;
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key, state_->market); one.has_value()) {
                views.push_back(*one);
            }
        }
        // depth first: with paced sends the last ones may never go
        const auto quote = topic_for(exec::SubKind::Bbo, state_->market).channel;
        std::ranges::stable_partition(views, [quote](const Subscription& one) { return one.channel == quote; });

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

    std::expected<void, exec::VenueError> watch_funding(std::string_view symbol) {
        for (const auto& existing : state_->funding_symbols) {
            if (existing == symbol) {
                return {};
            }
        }
        state_->funding_symbols.emplace_back(symbol);
        (void)state_->memo_for(symbol); // reconciliation reads only memos that exist
        return {};
    }

    void reconcile_marks(UnixNanos now) {
        if (state_->funding_symbols.empty() || marks_in_flight_ || (!marks_due_ && now.ns < next_marks_ns_)) {
            return;
        }
        if (!rest_.send(builder_.mark_prices(), [this](net::RestResponse&& response) { on_marks(response); })) {
            return;
        }
        marks_in_flight_ = true;
        marks_due_ = false;
        next_marks_ns_ = now.ns + k_mark_reconcile_interval_ns;
    }

    // index is asked after mark so a contract stale on both gets one record
    void on_marks(const net::RestResponse& response) noexcept {
        reconcile(response, &reconcile_mark);
        if (!rest_.send(builder_.index_prices(), [this](net::RestResponse&& next) { on_index(next); })) {
            publish_reconciled();
        }
    }

    void on_index(const net::RestResponse& response) noexcept {
        reconcile(response, &reconcile_index);
        publish_reconciled();
    }

    void
    reconcile(const net::RestResponse& response,
              bool (*apply)(const WireMarkPrice&, const exec::InstrumentTable&, FundingMemo&, UnixNanos)) noexcept {
        if (response.transport_failed()) {
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value() || envelope->return_code != k_ok) {
            return;
        }
        static thread_local const auto storage = std::make_unique<PriceRowBuffer>();
        const auto rows = parse_price_rows(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto now = state_->clock->wall();
        for (const auto& row : *rows) {
            if (auto* memo = state_->watched_memo(row.symbol); memo != nullptr) {
                (void)apply(row, state_->instruments, *memo, now);
            }
        }
    }

    void publish_reconciled() noexcept {
        marks_in_flight_ = false;
        const auto received = state_->clock->wall();
        std::size_t stale = 0;
        for (const auto& symbol : state_->funding_symbols) {
            auto* memo = state_->watched_memo(symbol);
            if (memo == nullptr) {
                continue;
            }
            if (const auto funding = map_reconciled(symbol, state_->instruments, *memo, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
                ++stale;
            }
        }
        RLOG_INFO("xt marks: {} contracts checked against rest, {} stale on the socket and taken from rest",
                  state_->funding_symbols.size(),
                  stale);
    }

    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_in_flight || state_->funding_symbols.empty() || now.ns < state_->next_funding_poll_ns) {
            return;
        }
        const auto count = state_->funding_symbols.size();
        const auto& symbol = state_->funding_symbols[state_->funding_cursor];
        state_->funding_cursor = (state_->funding_cursor + 1) % count;
        const auto spacing = std::max(k_funding_min_spacing_ns, k_funding_period_ns / count);
        state_->next_funding_poll_ns = now.ns + spacing;

        state_->funding_in_flight = true;
        VenueState* state = state_.get();
        if (!rest_.send(builder_.funding_rate(symbol), [state](net::RestResponse&& response) {
                state->funding_in_flight = false;
                on_funding_response(*state, response);
            })) {
            state_->funding_in_flight = false;
        }
    }

    static void on_funding_response(VenueState& state, const net::RestResponse& response) noexcept {
        if (response.transport_failed()) {
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value() || envelope->return_code != k_ok) {
            return;
        }
        const auto wire = parse_funding_rate(response.body);
        if (!wire.has_value()) {
            return;
        }
        const auto received = state.clock->wall();
        auto* memo = state.memo_for(wire->symbol);
        if (memo == nullptr) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, *memo, received); funding.has_value()) {
            state.sink->on_funding(*funding);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    RequestBuilder builder_;
    std::uint64_t next_marks_ns_{0};
    bool marks_in_flight_{false};
    bool marks_due_{true};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<XtData>(std::move(state));
}

} // namespace raikiri::venues::xt::internal
