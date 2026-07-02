#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::htx::detail {

namespace {

void handle_frame(VenueState& state, EventKind kind, std::string_view frame, UnixNanos received) {
    switch (kind) {
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
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Funding: {
        const auto wire = parse_funding(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Detail: {
        const auto wire = parse_detail(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto depth = map_depth(*wire, state.instruments, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarketPing:
    case EventKind::NotifyPing:
    case EventKind::SpotPing:
    case EventKind::AuthReply:
    case EventKind::PrivateOrder:
    case EventKind::PrivateMatch:
    case EventKind::PrivatePosition:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break; // the private kinds belong to the exec sockets, not this one
    }
}

// The market-data socket: bbo, trades and the step0 book.
class MarketHandler final : public net::WsHandler {
public:
    explicit MarketHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE, and nothing is sent in a loop. The pool
    // re-asks for the keys of the ONE connection that opened, PACED — which is
    // the whole fix: this used to push the entire catalogue's subscriptions
    // down a single socket as fast as the loop turned, and HTX answered 65 of
    // 340 contracts. One socket carries 346 paced topics perfectly well.
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
            if (event->kind == EventKind::MarketPing) {
                // BACK DOWN THE SOCKET THAT ASKED. HTX pings each connection
                // separately and wants that ping's own value echoed, so a
                // broadcast would answer one socket's ping on all of them and
                // leave the others' unanswered — which is how HTX decides a
                // socket has gone quiet.
                if (const auto pong = state_->protocol.market_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)pool_->reply(pong->text);
                }
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

// The notification socket: same host and gzip, a different dialect and a
// different keepalive spelling. Funding only — no other socket carries it.
class NotifyHandler final : public net::WsHandler {
public:
    explicit NotifyHandler(VenueState& state) : state_{&state} {}

    // Its own pool re-asks for this connection's codes; see the market handler.
    void on_open() override {}

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        try {
            const auto frame = inflater_.inflate(payload);
            if (!frame.has_value()) {
                ++state_->inflate_failures;
                return;
            }
            const auto event = parse_event(*frame);
            if (!event.has_value()) {
                return;
            }
            if (event->kind == EventKind::NotifyPing) {
                // BACK DOWN THE SOCKET THAT ASKED. HTX pings each connection
                // separately and wants that ping's own value echoed, so a
                // broadcast would answer one socket's ping on all of them and
                // leave the others' unanswered — which is how HTX decides a
                // socket has gone quiet.
                if (const auto pong = state_->protocol.notify_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)pool_->reply(pong->text);
                }
                return;
            }
            handle_frame(*state_, event->kind, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {}

    void attach(net::WsPool& pool) noexcept { pool_ = &pool; }

private:
    VenueState* state_;
    net::WsPool* pool_{nullptr};
    net::Inflater inflater_;
};

net::WsConfig socket_config(std::string_view path) {
    return {.host = std::string{stream_host()},
            .port = std::string{stream_port()},
            .target = std::string{path}};
}

// The topic is `market.<code>.<suffix>`, and a key here is the half after
// `market.`. SPLIT ON THE FIRST DOT: a suffix carries dots of its own
// ("trade.detail", "depth.step0") and a contract code never does, so the last
// dot would tear `trade.detail` in half.
std::string market_key(std::string_view code, std::string_view suffix) {
    return std::string{code} + "." + std::string{suffix};
}

class HtxData final : public exec::DataClient {
public:
    explicit HtxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, market_handler_{*state_},
          market_{state_->io,
                  socket_config(market_path()),
                  market_handler_,
                  net::WsPoolConfig{},
                  [this](std::span<const std::string> keys) { return market_frames(keys); }},
          notify_handler_{*state_},
          notify_{state_->io,
                  socket_config(notify_path()),
                  notify_handler_,
                  net::WsPoolConfig{},
                  [this](std::span<const std::string> keys) { return notify_frames(keys); }} {
        market_handler_.attach(market_);
        notify_handler_.attach(notify_);
    }

    std::expected<void, exec::VenueError> start() override {
        market_.start();
        notify_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        market_.stop();
        notify_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return market_.connected(); }

    // What this venue's sockets were told no about. Counted by the
    // adapter since it was written; this is what finally reads it.
    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override {
        return state_->subscribe_refusals;
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        if (instrument.market() != MarketType::Perp) {
            // api.hbdm.com is the futures tree. The spot host this adapter
            // knows is reached only for the private v2 stream — no public spot
            // market data is wired, and HTX spells the two markets
            // differently, so a spot code sent here is simply refused by the
            // venue after the subscription has been counted as successful.
            return std::unexpected{exec::not_implemented("htx spot market data")};
        }
        const auto code = state_->instruments.exchange_symbol(instrument);
        if (code.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding) {
            // The 24-hour turnover rides market.{code}.detail on the OTHER
            // socket: this one subscription covers both, because no SubKind
            // asks for a ticker and a caller that wanted funding wanted the
            // venue's own statistics.
            if (const auto detail = place(code, k_detail_topic); !detail.has_value()) {
                return detail; // a full pool is a refusal, not a quiet omission
            }
            for (const auto& existing : state_->funding_codes) {
                if (existing == code) {
                    return {};
                }
            }
            state_->funding_codes.emplace_back(code);
            // THE POOL PLACES IT, paced, on a socket with room or a new one.
            // A refusal is the pool's ceiling and is said out loud — and the
            // code comes back out of the desired set, or a reconnect would ask
            // for something no socket holds.
            const std::array<std::string, 1> one{std::string{code}};
            if (!notify_.subscribe(one)) {
                state_->funding_codes.pop_back();
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, "htx: notification pool is full")};
            }
            return {};
        }
        const auto suffix = topic_suffix(kind);
        if (suffix.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        return place(code, suffix);
    }

    std::size_t wait(std::uint32_t timeout_ms) noexcept override {
        return started_ ? state_->io.run_for(timeout_ms) : 0;
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        // WHERE THE PACING HAPPENS. Without these two the pools queue every
        // subscription and send none, which is the same dark board as before
        // by the opposite route.
        market_.poll(now);
        notify_.poll(now);
    }

private:
    std::expected<void, exec::VenueError> place(std::string_view code, std::string_view suffix) {
        const auto before = state_->desired.size();
        state_->add_subscription(code, suffix);
        if (state_->desired.size() == before) {
            return {}; // already placed on a socket
        }
        const std::array<std::string, 1> one{market_key(code, suffix)};
        if (!market_.subscribe(one)) {
            state_->desired.pop_back();
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "htx: market pool is full")};
        }
        return {};
    }

    // Called for one connection's keys, on its first open and on every reopen.
    [[nodiscard]] std::vector<std::string> market_frames(std::span<const std::string> keys) {
        std::vector<std::string> out;
        out.reserve(keys.size());
        for (const auto& key : keys) {
            const auto dot = key.find('.');
            if (dot == std::string::npos) {
                continue;
            }
            const auto frame = state_->protocol.subscribe_frame(
                std::string_view{key}.substr(0, dot), std::string_view{key}.substr(dot + 1));
            if (frame.has_value()) {
                out.emplace_back(frame->text);
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<std::string> notify_frames(std::span<const std::string> keys) {
        std::vector<std::string> out;
        out.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto frame = state_->protocol.funding_subscribe_frame(key);
                frame.has_value()) {
                out.emplace_back(frame->text);
            }
        }
        return out;
    }

    std::shared_ptr<VenueState> state_;
    MarketHandler market_handler_;
    net::WsPool market_;
    NotifyHandler notify_handler_;
    net::WsPool notify_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<HtxData>(std::move(state));
}

} // namespace raikiri::venues::htx::detail
