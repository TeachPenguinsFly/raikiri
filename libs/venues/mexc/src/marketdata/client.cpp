#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/mexc/book.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/market_data.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::mexc::internal {

namespace {

// MEXC closes a socket that has sent nothing for 60 s and counts only CLIENT
// messages toward that timer — verified: a connection receiving a push every
// three seconds was still dropped at 62 s. Ten seconds leaves room for a
// missed ping and a reconnect inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 10'000'000'000ULL;

constexpr std::uint64_t k_spot_ticker_poll_interval_ns = 300ULL * 1'000'000'000ULL;

// One spot frame. Protobuf, so nothing here is text and the JSON classifier
// never sees it: what the frame is is decided by WHICH payload field number is
// present, and an unknown one is skipped rather than guessed at.
//
// The subscription acks are JSON on the same socket. They start with '{' and
// no protobuf message can, so they cost one byte to tell apart.
void handle_spot_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    if (frame.empty() || frame.front() == '{') {
        return; // a control frame; the subscribe path already reports refusals
    }
    const auto event = parse_stream_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case StreamKind::Quote: {
        const auto quote = parse_stream_quote(event->payload);
        if (!quote.has_value()) {
            return;
        }
        if (const auto bbo =
                map_spot_bbo(*quote, event->symbol, event->ts_ms, state.instruments, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case StreamKind::Trade: {
        static thread_local StreamTradeBuffer trades;
        const auto parsed = parse_stream_trades(event->payload, trades);
        if (!parsed.has_value()) {
            return;
        }
        for (const auto& entry : *parsed) {
            if (const auto trade =
                    map_spot_trade(entry, event->symbol, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case StreamKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto book = parse_stream_book(event->payload, levels);
        if (!book.has_value()) {
            return;
        }
        if (const auto depth =
                map_spot_book(*book, event->symbol, event->ts_ms, state.instruments, received);
            depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case StreamKind::Unknown:
        break;
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer levels;
    const auto parsed = parse_event_and_depth(frame, levels);
    if (!parsed.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (parsed->event.kind) {
    case EventKind::DepthFull: {
        const auto& wire = parsed->depth;
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
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Depth:
        break;
    case EventKind::Error: {
        if (parse_stream_error(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::PersonalOrder:
    case EventKind::PersonalDeal:
    case EventKind::PersonalPosition:
    case EventKind::LoginAck:
        break; // private kinds belong to the execution client's own socket
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

// A pool key is one channel on one symbol, joined by a byte neither can
// contain. The pool stores keys as opaque strings and hands them back when a
// socket reconnects, so this is the only place that knows the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view symbol) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(symbol);
    return key;
}

// The level count belongs to the CHANNEL, not to the kind that asked for it: a
// key names a channel and a symbol, and the frame rebuilt from that key when a
// socket reopens must carry the limit the first ask carried. One rule, read by
// subscribe() and by split_key().
[[nodiscard]] std::uint32_t limit_for(std::string_view channel, MarketType market) noexcept {
    if (market == MarketType::Spot) {
        return channel == spot_channel_for(exec::SubKind::Depth) ? k_spot_book_depth : 0;
    }
    return channel == channel_for(exec::SubKind::Bbo) ? k_book_depth : 0;
}

// The inverse of key_of, and the only decode: two copies of where the cut is is
// one copy that drifts. Views into the key, which the pool owns for as long as
// the connection holds it.
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key, MarketType market) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    const auto channel = key.substr(0, cut);
    return Subscription{
        .channel = channel, .symbol = key.substr(cut + 1), .limit = limit_for(channel, market)};
}

// The channel carrying the top of book. On futures depth.full carries the
// ladder with it — one channel feeds both the Bid/Ask columns and the depth
// panel, because the incremental `depth` channel is parsed and dropped.
[[nodiscard]] std::string_view quote_channel(MarketType market) noexcept {
    return market == MarketType::Spot ? spot_channel_for(exec::SubKind::Bbo)
                                      : channel_for(exec::SubKind::Bbo);
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE. Each socket in the pool re-asks for its own
    // keys when it opens; replaying the whole desired set on every one of them
    // is the flood the pool exists to avoid, and this one handler is shared by
    // every connection so it cannot tell which of them called.
    //
    // NOTHING IS INVALIDATED HERE EITHER, and no OnConnectionOpen hook does it
    // elsewhere: the only book this adapter publishes is depth.full, which is a
    // limit-50 SNAPSHOT on every push, so a reopened socket's next frame is
    // already whole. The channel with a version chain is the incremental
    // `depth` one, and its frames are dropped — nothing here holds a chain a
    // reconnect could break, and there is no REST seed a resync could burst.
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        if (state_->market == MarketType::Spot) {
            handle_spot_frame(*state_, payload, received);
            return;
        }
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
};

class MexcData final : public exec::DataClient {
public:
    explicit MexcData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{private_rest_host()}, .port = "443"}},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->market)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path(state_->market)}},
                handler_,
                // The per-connection ceiling here is this adapter's share of a
                // socket rather than a number MEXC states; ws.hpp records what
                // one connection was measured to carry and why the share is
                // what bounds the ask.
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }} {
        spot_rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        if (state_->market == MarketType::Spot) {
            spot_rest_.start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        spot_rest_.stop();
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
            // One pool, one tree, and the trees do not even share an
            // encoding — a frame from the other one would not parse here.
            return std::unexpected{exec::not_implemented("mexc: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const bool spot = state_->market == MarketType::Spot;
        if (spot && kind == exec::SubKind::Funding) {
            if (std::ranges::find(ticker_instruments_, instrument) == ticker_instruments_.end()) {
                ticker_instruments_.push_back(instrument);
            }
            return {};
        }
        auto channel = spot ? spot_channel_for(kind) : channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (!spot && kind == exec::SubKind::Depth) {
            // The ladder arrives on depth.full, a whole snapshot per push, and
            // handle_frame drops every frame MEXC's incremental `depth` channel
            // sends. Asking for it anyway was a quarter of the pool — 1183 keys
            // and about two and a half sockets — spent on frames that are
            // parsed and thrown away.
            channel = channel_for(exec::SubKind::Bbo);
        }
        const auto before = state_->desired.size();
        state_->add_subscription(channel, symbol, limit_for(channel, state_->market));
        if (state_->desired.size() == before) {
            return {}; // funding and the mark are ONE `ticker` channel, not two
        }
        // THE POOL PLACES IT. A key that fits on no existing socket opens
        // another, and only the pool's own ceiling refuses — which is said out
        // loud rather than leaving the instrument silently unwatched.
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "mexc: connection pool is full")};
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
        poll_spot_ticker(now);
        if (now.ns < state_->next_app_ping_ns) {
            return;
        }
        // EVERY socket. MEXC's 60 s idle timer counts only CLIENT messages and
        // runs per connection, so pinging one of sixteen has the venue close
        // the other fifteen a minute later.
        pool_.broadcast(Protocol::ping_frame());
        if (state_->private_client != nullptr && state_->private_client->connected()) {
            (void)state_->private_client->send(Protocol::ping_frame());
        }
        state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
    }

private:
    void poll_spot_ticker(UnixNanos now) noexcept {
        if (ticker_instruments_.empty() || now.ns < next_ticker_poll_ns_ || ticker_in_flight_) {
            return;
        }
        ticker_in_flight_ = true;
        try {
            if (!spot_rest_.send(state_->builder.spot_ticker_24h(),
                                 [this](const net::RestResponse& response) {
                                     on_spot_ticker(response);
                                 })) {
                ticker_in_flight_ = false;
                return;
            }
        } catch (...) {
            ticker_in_flight_ = false;
            return;
        }
        next_ticker_poll_ns_ = now.ns + k_spot_ticker_poll_interval_ns;
    }

    void on_spot_ticker(const net::RestResponse& response) noexcept {
        ticker_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<SpotTickerBuffer>();
        const auto rows = parse_spot_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto stats = map_spot_ticker_stats(row, state_->instruments, received);
            if (stats.has_value() &&
                std::ranges::find(ticker_instruments_, stats->instrument) !=
                    ticker_instruments_.end()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the ordering lives here
    // rather than at subscribe time: on a reconnect the order that matters is
    // the order the venue is asked in.
    //
    // ONE FRAME PER KEY. `param` names a single symbol on futures and `params`
    // a single channel on spot, so a connection's set costs its key count in
    // sends — which is what the venues table's ws_streams bounds.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        const bool spot = state_->market == MarketType::Spot;
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key, state_->market); one.has_value()) {
                views.push_back(*one);
            }
        }
        // THE QUOTE FIRST, then the rest. Once the sends are paced, whatever
        // goes last effectively does not go at all — and on futures depth.full
        // is both the Bid/Ask columns and the only ladder this adapter
        // publishes, so it is the one that must not end up behind 400 others.
        const auto quote = quote_channel(state_->market);
        std::ranges::stable_partition(
            views, [quote](const Subscription& one) { return one.channel == quote; });

        std::vector<std::string> frames;
        frames.reserve(views.size());
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame = spot ? state_->protocol.spot_subscribe_frame(views[built])
                                    : state_->protocol.subscribe_frame(
                                          std::span<const Subscription>{views}.subspan(built));
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
    net::RestClient spot_rest_;
    net::WsPool pool_;
    std::vector<InstrumentId> ticker_instruments_;
    std::uint64_t next_ticker_poll_ns_{0};
    bool ticker_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<MexcData>(std::move(state));
}

} // namespace raikiri::venues::mexc::internal
