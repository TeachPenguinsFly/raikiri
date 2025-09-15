#pragma once

// The private socket, and only that.
//
// It subscribes and turns each frame into an event on the sink. It sends no
// orders: an order goes out over REST and is ANSWERED here, and joining those
// two halves is the execution client's job, not this one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <array>
#include <charconv>
#include <span>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::gate::detail {

constexpr std::uint32_t k_private_stream_id = 1;

// Routes one private frame. The market decides which of Gate's two order
// shapes the payload is; nothing in the frame itself says.
void handle_private_frame(VenueState& state,
                          MarketType market,
                          std::string_view frame,
                          UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::OrderUpdate: {
        static thread_local OrderBuffer orders;
        const auto wire = parse_ws_orders(frame, orders, market);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            // Remembered BEFORE the event is emitted: a user trade for the
            // same fill may already be queued behind this frame, and it can
            // only state its leaves out of this memo.
            state.remember(entry);
            if (const auto mapped = map_order_event(entry, state.instruments, received);
                mapped.has_value()) {
                state.sink->on_order_event(*mapped);
            }
        }
        break;
    }
    case EventKind::UserTrade: {
        static thread_local UserTradeBuffer trades;
        const auto wire = parse_ws_user_trades(frame, trades, market);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            const auto memo = state.memo_for(entry.order_id);
            if (const auto mapped = map_user_trade(
                    entry, state.instruments, memo.leaves, memo.client_order_id, received);
                mapped.has_value()) {
                state.sink->on_order_event(*mapped);
            }
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            // Code 4 wraps a REST-style label in its message; the number is
            // enough to tell a bad key from an unknown contract.
            state.sink->on_connectivity(
                {.state = classify_stream(err->code) == exec::ErrorClass::AuthFailure
                              ? exec::ConnState::AuthFailed
                              : exec::ConnState::Connected,
                 .stream_id = k_private_stream_id,
                 .ts = state.clock->wall()});
        }
        break;
    }
    case EventKind::BookTicker:
    case EventKind::Trade:
    case EventKind::Ticker:
    case EventKind::BookUpdate:
    case EventKind::BookSnapshot:
    case EventKind::Pong:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break;
    }
}

// One private socket. Futures and spot are different hosts carrying different
// order shapes, so an account trading both holds two of these.
class PrivateHandler final : public net::WsHandler {
public:
    PrivateHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        if (!subscribe(orders_channel(market_)) || !subscribe(user_trades_channel(market_))) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, market_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    [[nodiscard]] bool subscribe(std::string_view channel) {
        if (channel.empty() || client_ == nullptr) {
            return false;
        }
        // The user id is positional and futures-only: sending the contract
        // where Gate expects the id subscribes to nothing and reports success.
        std::array<char, 24> id_text{};
        std::array<std::string_view, 2> payload{};
        std::size_t count = 0;
        if (market_ == MarketType::Perp) {
            const auto [stop, ec] = std::to_chars(
                id_text.data(), id_text.data() + id_text.size(), state_->futures_user_id);
            if (ec != std::errc{} || state_->futures_user_id == 0) {
                return false;
            }
            payload[count++] = {id_text.data(), static_cast<std::size_t>(stop - id_text.data())};
        }
        payload[count++] = k_all_symbols;

        const auto frame = state_->protocol.private_subscribe_frame(
            channel, std::span{payload}.first(count), state_->signer, state_->now_s());
        return frame.has_value() && client_->send(*frame);
    }

    VenueState* state_;
    MarketType market_;
    net::WsClient* client_{nullptr};
};

// The socket itself, owned as one thing so a caller cannot hold the handler
// without the client it is attached to.
class PrivateStream {
public:
    PrivateStream(VenueState& state, MarketType market)
        : handler_{state, market},
          ws_{state.io,
              net::WsConfig{.host = std::string{private_host(state.testnet, market)},
                            .port = std::string{stream_port()},
                            .target = std::string{private_path(state.testnet, market)}},
              handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

} // namespace raikiri::venues::gate::detail
