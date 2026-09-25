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

namespace raikiri::venues::bitfinex::internal {

namespace {

// api-pub allows 20 new connections a minute and the pool reopens every socket at once
constexpr std::size_t k_max_connections = 18;

// pool key is channel and symbol joined by a byte neither contains
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(ChannelKind kind, std::string_view symbol) {
    std::string key{channel_name(kind)};
    key.push_back(k_key_sep);
    key.append(symbol);
    return key;
}

// the only decode; the symbol views the pool-owned key
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    const auto channel = key.substr(0, cut);
    // status carries several feeds; deriv: is the only one requested
    const auto kind = channel == channel_name(ChannelKind::DerivStatus) ? ChannelKind::DerivStatus
                                                                        : channel_kind(channel, {});
    if (kind == ChannelKind::Unknown) {
        return std::nullopt;
    }
    return Subscription{.kind = kind, .symbol = key.substr(cut + 1)};
}

// only this socket's ids and books are invalidated
void forget(VenueState& state, ChannelRegistry& registry) noexcept {
    for (const auto& binding : registry.bound()) {
        if (binding.kind != ChannelKind::Book) {
            continue;
        }
        const auto* instrument = state.instruments.find(binding.symbol.view());
        if (instrument == nullptr) {
            continue;
        }
        if (const auto entry = state.books.find(base_of(*instrument).id.packed); entry != state.books.end()) {
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
    const auto text = kind == ChannelKind::DerivStatus ? symbol_from_deriv_key(ack->key) : ack->symbol;
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
        if (const auto bbo = map_bbo(*wire, binding->symbol.view(), state.instruments, ts_event, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto stats = map_ticker_stats(*wire, binding->symbol.view(), state.instruments, ts_event, received);
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
            if (const auto trade = map_trade(entry, binding->symbol.view(), state.instruments, received);
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
        if (const auto funding = map_funding(*wire, binding->symbol.view(), state.instruments, received);
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
        // book frames have no timestamp, so ts_event is ingest time
        if (const auto depth = book.snapshot(received, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        // the fast top of book, beside the ticker's
        if (const auto bbo = book.touch(received, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case ChannelKind::Unknown:
        break;
    }
}

// noexcept though it allocates; an escape here aborts
void handle_frame(VenueState& state, ChannelRegistry& registry, std::string_view frame, UnixNanos received) noexcept {
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
        // both notices reissue channel ids on this socket only
        if (info->code == k_info_reconnect || info->code == k_info_maintenance_end) {
            forget(state, registry);
        }
        break;
    case FrameKind::Error: {
        // 10305: the socket was asked for more channels than its server serves
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case FrameKind::Auth:
        // only the execution client's socket authenticates
        break;
    case FrameKind::Heartbeat:
    case FrameKind::Unsubscribed:
    case FrameKind::Pong:
    case FrameKind::Unknown:
        break;
    }
}

// one connection's handler; there is a pool per socket
class SocketHandler final : public net::WsHandler {
public:
    SocketHandler(VenueState& state, ChannelRegistry& registry) : state_{&state}, registry_{&registry} {}

    // the pool resubscribes on open, which reseeds each book with a snapshot
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
        return std::ranges::any_of(sockets_, [](const auto& socket) { return socket->pool->connected(); });
    }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "symbol too long")};
        }
        if (!state_->add_subscription(channel, *owned)) {
            return {}; // funding and mark price share one status key
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
            // ping every socket; one with no ack yet gets no heartbeat
            socket->pool->broadcast(Protocol::ping_frame());
            pinged = true;
        }
        if (pinged) {
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        // the execution client's socket rides this timer too
        if (state_->private_poll) {
            state_->private_poll(now);
        }
    }

private:
    // one pool per socket: frames carry only a chanId, valid on the socket that acked it
    struct Socket {
        explicit Socket(VenueState& state) : handler{state, registry} {}

        ChannelRegistry registry{};
        SocketHandler handler;
        std::unique_ptr<net::WsPool> pool; // last: it holds the handler above
    };

    // first socket with room, else a new one; only the connection ceiling refuses
    std::expected<void, exec::VenueError> place(std::string key) {
        const std::array<std::string, 1> one{std::move(key)};
        for (const auto& socket : sockets_) {
            if (socket->pool->subscribe(one)) {
                return {};
            }
        }
        if (sockets_.size() >= k_max_connections) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitfinex: connection pool is full")};
        }
        auto& opened = *sockets_.emplace_back(make_socket());
        if (started_) {
            opened.pool->start();
        }
        if (!opened.pool->subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitfinex: a fresh socket refused a key")};
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
            // one connection, matching the per-socket registry
            net::WsPoolConfig{.max_connections = 1},
            [this](std::span<const std::string> keys) { return build_frames(keys); });
        return socket;
    }

    // called on every open of the socket; one frame per key
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        // book first: with paced sends the tail lags, and the book feeds the bbo
        std::ranges::stable_partition(views, [](const Subscription& one) { return one.kind == ChannelKind::Book; });

        std::vector<std::string> frames;
        frames.reserve(views.size() + 1);
        // conf goes before any subscription on every open; the parsers expect its number format
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

} // namespace raikiri::venues::bitfinex::internal
