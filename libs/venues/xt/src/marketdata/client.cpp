#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
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

namespace raikiri::venues::xt::detail {

namespace {

// XT streams no funding channel, so the rate is polled. One symbol is rotated
// per tick and the whole set is covered every period; the spacing floor keeps
// a large catalogue from bursting. Funding is a slow datum — the rate in force
// changes on a settlement boundary hours away — so a long period costs nothing.
constexpr std::uint64_t k_funding_period_ns = 10'000'000'000ULL;

constexpr std::uint64_t k_funding_min_spacing_ns = 250'000'000ULL;

// Inside XT's 30 s idle window with room for a missed one and a reconnect.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// One send carries up to k_max_subscriptions_per_frame topics, so a
// connection's whole ask is its key count divided by 32 — a 150 ms floor
// covers the futures tree's fullest socket in under three seconds. XT states
// no message rate for either public socket; this is the pacing every other
// pooled venue here runs at, spent per connection rather than once.
constexpr std::uint64_t k_subscribe_spacing_ns = 150ULL * 1'000'000ULL;

// Enough sockets for either tree at 192 keys each: the futures catalogue's 657
// contracts across four channels — depth, ticker, trade and mark_price — is
// 2628 keys and takes 14, and spot's 486 pairs across three takes 8. Room for
// 1152 contracts, and under what one IP is served: 24 connections opened on
// fstream and 22 were still up 75 s later, so the ceiling that would hurt to
// trip is not this one.
constexpr std::size_t k_max_connections = 24;

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
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::DepthUpdate:
        // Not subscribed: the book comes off the `depth` snapshot. Named here
        // rather than folded into Unknown so a frame arriving anyway is a
        // recognised one and not a mystery.
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
        if (const auto funding = map_mark_price(*wire, state.instruments, *memo, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Ack:
    case EventKind::Unknown:
        break;
    }
}

// A pool key is one channel on one symbol, joined by a byte neither can
// contain. The pool stores keys as opaque strings and hands them back when a
// socket reconnects, so this is the only place that knows the shape. The
// topic's trailing params are NOT carried: they belong to the channel and the
// tree, and params_for is the one rule both sides read.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view symbol) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(symbol);
    return key;
}

// What XT requires after the symbol, by CHANNEL rather than by the kind that
// asked for it: a frame rebuilt from a key when a socket reopens must carry
// the same suffix the first ask carried, and `depth` without its level count
// is a different subscription.
[[nodiscard]] std::string_view params_for(std::string_view channel, MarketType market) noexcept {
    const auto depth = topic_for(exec::SubKind::Bbo, market);
    return channel == depth.channel ? depth.params : std::string_view{};
}

// The inverse of key_of, and the only decode: two readers of a key that
// disagree about where the cut is drift apart silently. Views into the key,
// which the pool owns for as long as the connection holds it.
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key, MarketType market) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    const auto channel = key.substr(0, cut);
    return Subscription{
        .channel = channel, .symbol = key.substr(cut + 1), .params = params_for(channel, market)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE. Each socket in the pool re-asks for its own
    // keys when it opens; replaying the whole desired set on every one of them
    // is the flood the pool exists to avoid, and this one handler is shared by
    // every connection so it cannot tell which of them called.
    //
    // NOTHING IS INVALIDATED HERE EITHER, and the pool is given no
    // OnConnectionOpen to do it elsewhere: the only book this adapter
    // publishes is `depth`, a self-contained top-50 snapshot on a fixed
    // cadence, so a reopened socket's next frame is already whole. The channel
    // with a chain is `depth_update`, which is not subscribed — nothing here
    // holds a sequence a reconnect could break, and there is no REST seed a
    // resync could burst.
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
                // The ceiling is a CONNECTION's, not the process's — reading it
                // as the process's left 342 of 486 spot pairs watched, and
                // ws.hpp has what one socket was measured to carry.
                net::WsPoolConfig{.max_keys_per_connection = k_max_subscriptions_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

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
            // One pool, one deployment. XT spells a spot pair and its
            // perpetual identically, so an id from the other market would be
            // accepted here and answered with the wrong tree's prices.
            return std::unexpected{exec::not_implemented("xt: this pool serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            if (state_->market != MarketType::Perp) {
                // A spot pair pays no funding. The poll is a REST call on the
                // futures tree, and left unguarded it answered a spot gateway
                // with the PERPETUAL's rate under a spot instrument id.
                return std::unexpected{exec::not_implemented("xt: spot pays no funding")};
            }
            // The mark rides with it: XT streams it on a channel of its own,
            // `FundingUpdate` carries both, and nothing else opens that
            // channel.
            if (const auto placed =
                    want(topic_for(exec::SubKind::MarkPrice, state_->market), symbol);
                !placed.has_value()) {
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
            // Rides with the BBO rather than with funding: XT streams no
            // funding channel, so nothing else is asked for on both trees.
            return want(ticker_topic(), symbol);
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        pool_.poll(now);
        if (now.ns >= state_->next_app_ping_ns) {
            // EVERY socket. The idle timer is per connection and counts only
            // what the client sent, so pinging one of fourteen leaves the other
            // thirteen to be closed on schedule — which the single socket this
            // replaces was already suffering, three times in a 90 s run, each
            // one costing the whole catalogue's resubscription.
            pool_.broadcast(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        poll_funding(now);
        // The execution client's private sockets ride this timer too:
        // ExecutionClient has no poll hook, and XT closes a socket that has
        // sent nothing for 30 s however much it received.
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    // Records the topic for the next reconnect and hands the key to the pool,
    // which places it on a connection with room or opens another. Only the
    // pool's own ceiling refuses — said out loud rather than leaving the
    // instrument silently unwatched.
    std::expected<void, exec::VenueError> want(const Topic& topic, std::string_view symbol) {
        if (!state_->add_subscription(topic.channel, symbol)) {
            return {}; // already asked for; refcounting lives in the gateway core
        }
        const std::array<std::string, 1> one{key_of(topic.channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "xt: connection pool is full")};
        }
        return {};
    }

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the ordering lives
    // here rather than at subscribe time: on a reconnect the order that
    // matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key, state_->market); one.has_value()) {
                views.push_back(*one);
            }
        }
        // THE QUOTE FIRST. Once the sends are paced, whatever goes last
        // effectively does not go at all — and `depth` is both the Bid/Ask
        // columns and the only ladder this adapter publishes.
        const auto quote = topic_for(exec::SubKind::Bbo, state_->market).channel;
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

    std::expected<void, exec::VenueError> watch_funding(std::string_view symbol) {
        for (const auto& existing : state_->funding_symbols) {
            if (existing == symbol) {
                return {};
            }
        }
        state_->funding_symbols.emplace_back(symbol);
        return {};
    }

    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_in_flight || state_->funding_symbols.empty() ||
            now.ns < state_->next_funding_poll_ns) {
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
        if (const auto funding = map_funding(*wire, state.instruments, *memo, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    RequestBuilder builder_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<XtData>(std::move(state));
}

} // namespace raikiri::venues::xt::detail
