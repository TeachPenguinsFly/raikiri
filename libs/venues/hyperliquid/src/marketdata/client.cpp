#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::hyperliquid::internal {

namespace {

// the venue closes a socket idle for 60 s and control frames do not reset that timer
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// one dex per read at weight 20 is 400 of the ip's 1200 a minute
constexpr std::uint64_t k_context_poll_interval_ns = 3'000'000'000ULL;

void publish_context(VenueState& state, const WireAssetCtx& wire, UnixNanos received) {
    // spot contexts carry no funding, so only turnover is published for them
    if (const auto funding = map_funding(wire, state.instruments, received); funding.has_value()) {
        state.sink->on_funding(*funding);
    }
    if (const auto stats = map_ticker_stats(wire, state.instruments, received); stats.has_value()) {
        state.sink->on_ticker_stats(*stats);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto kind = parse_event(frame);
    if (!kind.has_value()) {
        return; // unknown shapes are ignored
    }
    switch (*kind) {
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
            if (const auto trade = map_trade(entry, state.instruments, received); trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::AssetCtx: {
        if (const auto wire = parse_asset_ctx(frame); wire.has_value()) {
            publish_context(state, *wire, received);
        }
        break;
    }
    case EventKind::Book: {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(frame, storage);
        if (!wire.has_value()) {
            return;
        }
        // most pairs never send bbo, so level zero of each l2Book side supplies the quote
        if (!wire->bids.empty() && !wire->asks.empty()) {
            const WireBbo top{.coin = wire->coin,
                              .bid_price = wire->bids.front().price,
                              .bid_qty = wire->bids.front().qty,
                              .ask_price = wire->asks.front().price,
                              .ask_qty = wire->asks.front().qty,
                              .ts_ms = wire->ts_ms};
            if (const auto bbo = map_bbo(top, state.instruments, received); bbo.has_value()) {
                state.sink->on_bbo(*bbo);
            }
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        if (parse_stream_error(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

// pool key is channel and coin joined by a byte neither contains
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view coin) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(coin);
    return key;
}

// views into the key, owned by the pool while the connection holds it
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Subscription{.channel = key.substr(0, cut), .coin = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // each pool socket resubscribes its own keys on open; l2Book frames
    // are full snapshots, so nothing else is needed
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

class HyperliquidData final : public exec::DataClient {
public:
    explicit HyperliquidData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // 1000 subscriptions per socket and usually per ip; see the venues table
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {
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
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = stream_channel(kind, state_->market);
        if (channel.empty() && !channel_for(kind).empty()) {
            watch_context(symbol);
            return {};
        }
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (!state_->add_subscription(channel, symbol)) {
            return {}; // spot funding and mark price share one activeAssetCtx key
        }
        // the pool opens another socket when none has room
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "hyperliquid: no room under the ip subscription cap")};
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
        poll_contexts(now);
        if (pool_.connected() && now.ns >= state_->next_app_ping_ns) {
            // ping every socket; the idle timer is per connection
            pool_.broadcast(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    void watch_context(std::string_view coin) {
        context_coins_.emplace(coin);
        const auto dex = dex_of(coin);
        if (std::ranges::find(context_dexes_, dex) == context_dexes_.end()) {
            context_dexes_.emplace_back(dex);
        }
    }

    void poll_contexts(UnixNanos now) noexcept {
        if (context_dexes_.empty() || context_in_flight_ || now.ns < next_context_poll_ns_) {
            return;
        }
        next_context_poll_ns_ = now.ns + k_context_poll_interval_ns;
        const auto& dex = context_dexes_[context_cursor_];
        context_cursor_ = (context_cursor_ + 1) % context_dexes_.size();
        context_in_flight_ = true;
        try {
            if (!rest_.send(RequestBuilder::meta_and_asset_ctxs(dex, context_body_),
                            [this](const net::RestResponse& response) { on_contexts(response); })) {
                context_in_flight_ = false;
            }
        } catch (...) {
            context_in_flight_ = false;
        }
    }

    void on_contexts(const net::RestResponse& response) noexcept {
        context_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return; // the next turn reads again; a missed rate is not an incident
        }
        const auto rows = parse_asset_ctxs(response.body, *contexts_);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            if (context_coins_.contains(row.coin)) {
                publish_context(*state_, row, received);
            }
        }
    }

    // runs on every open of the socket, so bbo is ordered first here; one frame per key
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        // bbo first, since paced sends at the tail may never go out
        const auto quote = channel_for(exec::SubKind::Bbo);
        std::ranges::stable_partition(views, [quote](const Subscription& one) { return one.channel == quote; });

        std::vector<std::string> frames;
        frames.reserve(views.size());
        for (const auto& one : views) {
            // copy before the next key overwrites the builder's buffer
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                frames.emplace_back(*frame);
            }
        }
        return frames;
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    std::string context_body_;
    std::unique_ptr<AssetCtxBuffer> contexts_{std::make_unique<AssetCtxBuffer>()};
    std::set<std::string, std::less<>> context_coins_;
    std::vector<std::string> context_dexes_;
    std::size_t context_cursor_{0};
    std::uint64_t next_context_poll_ns_{0};
    bool context_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<HyperliquidData>(std::move(state));
}

} // namespace raikiri::venues::hyperliquid::internal
