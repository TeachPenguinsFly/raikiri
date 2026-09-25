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

namespace raikiri::venues::bingx::internal {

namespace {

// funding is polled one symbol per tick
constexpr std::uint64_t k_funding_poll_interval_ns = 2'000'000'000ULL;

// pool key is symbol and suffix joined by a byte neither contains
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
        if (const auto bbo = map_bbo(*wire, state.instruments, state.market, received); bbo.has_value()) {
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
            if (const auto trade = map_trade(entry, state.instruments, state.market, received); trade.has_value()) {
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
        if (const auto book = map_book(*wire, state.instruments, state.market, received); book.has_value()) {
            state.sink->on_depth(*book);
            if (state.market != MarketType::Spot) {
                if (const auto touch = touch_of(*book); touch.has_value()) {
                    state.sink->on_bbo(*touch);
                }
            }
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, state.market, received); stats.has_value()) {
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

    // each socket resubscribes its own keys; depth frames are full snapshots, so no book to invalidate
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
                // bingx pings every connection and the handler cannot tell which, so pong goes to all
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
                // bingx caps a connection at 200 topics
                net::WsPoolConfig{},
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

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // spot and perp share symbols; one socket serves one market
            return std::unexpected{exec::not_implemented("bingx: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            // funding is polled over rest; the ticker stream is subscribed alongside
            if (const auto placed = subscribe_channel(symbol, k_ticker_channel); !placed.has_value()) {
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
        // this is the exec client's only tick
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    std::expected<void, exec::VenueError> subscribe_channel(std::string_view symbol, std::string_view suffix) {
        const auto before = state_->desired.size();
        state_->add_subscription(symbol, suffix);
        if (state_->desired.size() == before) {
            return {}; // refcounting lives in the gateway core; this is a set
        }
        // the pool places the key and refuses only at its own ceiling
        const std::array<std::string, 1> one{key_of(symbol, suffix)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bingx: connection pool is full")};
        }
        return {};
    }

    // called on every open of the socket; bookTicker goes first
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
            views.push_back(
                {.symbol = std::string_view{key}.substr(0, cut), .suffix = std::string_view{key}.substr(cut + 1)});
        }
        // quote first: with paced sends the tail of a large catalogue lags
        const auto quote = data_type_suffix(exec::SubKind::Bbo, state_->market);
        std::ranges::stable_partition(views, [quote](const View& one) { return one.suffix == quote; });

        // one frame per topic; copied because the protocol buffer is reused
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
        if (state_->funding_symbols.empty() || now.ns < state_->next_funding_poll_ns || funding_in_flight_) {
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
        if (const auto funding = map_funding(*wire, state_->instruments, received); funding.has_value()) {
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

} // namespace raikiri::venues::bingx::internal
