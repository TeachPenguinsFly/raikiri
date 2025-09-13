#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

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

    void on_open() override {
        for (const auto& entry : state_->desired) {
            const auto frame = state_->protocol.subscribe_frame(entry.code, entry.suffix);
            if (!frame.has_value()) {
                continue;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
        }
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
                if (const auto pong = state_->protocol.market_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
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

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

// The notification socket: same host and gzip, a different dialect and a
// different keepalive spelling. Funding only — no other socket carries it.
class NotifyHandler final : public net::WsHandler {
public:
    explicit NotifyHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& code : state_->funding_codes) {
            const auto frame = state_->protocol.funding_subscribe_frame(code);
            if (!frame.has_value()) {
                continue;
            }
            if (!client_->send(frame->text)) {
                break;
            }
        }
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
                return;
            }
            if (event->kind == EventKind::NotifyPing) {
                if (const auto pong = state_->protocol.notify_pong_frame(event->keepalive_ts);
                    pong.has_value()) {
                    (void)client_->send(pong->text);
                }
                return;
            }
            handle_frame(*state_, event->kind, *frame, received);
        } catch (...) {
            ++state_->inflate_failures;
        }
    }

    void on_close(net::WsCloseReason) override {}

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
    net::Inflater inflater_;
};

net::WsConfig socket_config(std::string_view path) {
    return {.host = std::string{stream_host()},
            .port = std::string{stream_port()},
            .target = std::string{path}};
}

class HtxData final : public exec::DataClient {
public:
    explicit HtxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, market_handler_{*state_},
          market_{state_->io, socket_config(market_path()), market_handler_},
          notify_handler_{*state_},
          notify_{state_->io, socket_config(notify_path()), notify_handler_} {
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
            subscribe_detail(code);
            for (const auto& existing : state_->funding_codes) {
                if (existing == code) {
                    return {};
                }
            }
            state_->funding_codes.emplace_back(code);
            if (notify_.connected()) {
                if (const auto frame = state_->protocol.funding_subscribe_frame(code);
                    frame.has_value()) {
                    (void)notify_.send(frame->text);
                }
            }
            return {};
        }
        const auto suffix = topic_suffix(kind);
        if (suffix.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        state_->add_subscription(code, suffix);
        if (market_.connected()) {
            if (const auto frame = state_->protocol.subscribe_frame(code, suffix);
                frame.has_value()) {
                (void)market_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
    }

private:
    void subscribe_detail(std::string_view code) {
        state_->add_subscription(code, k_detail_topic);
        if (market_.connected()) {
            if (const auto frame = state_->protocol.subscribe_frame(code, k_detail_topic);
                frame.has_value()) {
                (void)market_.send(frame->text);
            }
        }
    }

    std::shared_ptr<VenueState> state_;
    MarketHandler market_handler_;
    net::WsClient market_;
    NotifyHandler notify_handler_;
    net::WsClient notify_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<HtxData>(std::move(state));
}

} // namespace raikiri::venues::htx::detail
