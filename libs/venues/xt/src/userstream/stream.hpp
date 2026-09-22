#pragma once

// The private socket, and only that.
//
// It subscribes with the key it was handed, keeps the connection alive and
// turns each push into an event on the sink. It sends no orders: an order goes
// out over REST and is ANSWERED here, and joining those two halves is the
// execution client's job, not this one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/venues/xt/ws.hpp>

#include <cstdint>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::xt::internal {

// XT closes a private socket that has sent no application `ping` for 30 s, and
// only CLIENT messages count toward that timer — a connection receiving order
// pushes the whole time is still dropped. Ten seconds of headroom leaves room
// for a missed tick and a reconnect inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

constexpr std::uint32_t k_perp_stream_id = 1;

constexpr std::uint32_t k_spot_stream_id = 2;

// Routes one private push. Both trees wear the same {topic,event,data} shell
// and NOTHING ELSE in common: futures states an order with long field names,
// spot with two-letter ones, so the reader is chosen by the market rather than
// sniffed from the payload.
void handle_private_frame(VenueState& state,
                          MarketType market,
                          std::string_view frame,
                          UnixNanos received) {
    const auto info = parse_private_frame(frame);
    if (!info.has_value()) {
        return; // the `pong` answer is bare text, not JSON
    }
    const auto stream_id = market == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
    switch (info->topic) {
    case PrivateTopic::Order: {
        if (market == MarketType::Spot) {
            const auto wire = parse_spot_order_push(frame);
            if (!wire.has_value()) {
                return;
            }
            // Remembered BEFORE the event is emitted: a fill for the same order
            // may already be queued behind this frame, and it can state neither
            // its side nor its leaves out of anything else.
            OrderContext context = state.context_for(wire->order_id);
            context.side = wire->side == "SELL" ? Side::Sell : Side::Buy;
            context.side_known = !wire->side.empty();
            if (const auto report = map_spot_open_order(*wire, state.instruments);
                report.has_value()) {
                context.leaves = report->leaves;
                if (!report->exchange_client_order_id.empty()) {
                    context.client_order_id = report->exchange_client_order_id;
                }
            }
            state.remember(wire->order_id, context);
            if (const auto event = map_spot_order_event(*wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
            return;
        }
        const auto wire = parse_futures_order(frame);
        if (!wire.has_value()) {
            return;
        }
        OrderContext context = state.context_for(wire->order_id);
        context.side = wire->order_side == "SELL" ? Side::Sell : Side::Buy;
        context.side_known = !wire->order_side.empty();
        if (const auto report = map_futures_open_order(*wire, state.instruments);
            report.has_value()) {
            context.leaves = report->leaves;
            if (!report->exchange_client_order_id.empty()) {
                context.client_order_id = report->exchange_client_order_id;
            }
        }
        state.remember(wire->order_id, context);
        if (const auto event = map_futures_order_event(
                *wire, state.instruments, context.client_order_id, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    case PrivateTopic::Trade: {
        if (market == MarketType::Spot) {
            const auto wire = parse_spot_trade_push(frame);
            if (!wire.has_value()) {
                return;
            }
            if (const auto event = map_spot_trade(
                    *wire, state.instruments, state.context_for(wire->order_id), received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
            return;
        }
        const auto wire = parse_futures_deal(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto event = map_futures_deal(
                *wire, state.instruments, state.context_for(wire->order_id), received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    case PrivateTopic::Position:
    case PrivateTopic::Balance:
        // GatewaySink carries no channel for either; the reconciler asks for
        // positions through position_reports() instead.
        return;
    case PrivateTopic::Ack:
        state.sink->on_connectivity({.state = exec::ConnState::Connected,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateTopic::Refusal:
        // Code 2 is the only refusal with a remedy: the key is dead and a new
        // one has to be minted before the socket can carry anything.
        if (info->code == k_listen_key_invalid) {
            (market == MarketType::Spot ? state.spot_key_stale : state.perp_key_stale) = true;
        }
        state.last_error.assign(info->msg);
        state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateTopic::Unknown:
        return;
    }
}

class PrivateHandler final : public net::WsHandler {
public:
    PrivateHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        const auto stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
        const auto frame = market_ == MarketType::Spot ? protocol_.spot_subscribe(*key_)
                                                       : protocol_.futures_subscribe(*key_);
        if (!frame.has_value() || client_ == nullptr || !client_->send(*frame)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, market_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected,
             .stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id,
             .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client, const std::string& key) noexcept {
        client_ = &client;
        key_ = &key;
    }

private:
    VenueState* state_;
    MarketType market_;
    PrivateProtocol protocol_;
    net::WsClient* client_{nullptr};
    const std::string* key_{nullptr};
};

// One private connection, owned as one thing so a caller cannot hold the
// handler without the client it is attached to. The key is owned here rather
// than by the handler because it is re-minted over REST between reconnects.
class PrivateStream {
public:
    PrivateStream(VenueState& state, MarketType market)
        : handler_{state, market},
          client_{state.io,
                  net::WsConfig{
                      .host = std::string{market == MarketType::Spot ? spot_private_stream_host()
                                                                     : private_stream_host()},
                      .port = "443",
                      .target = std::string{market == MarketType::Spot ? spot_private_stream_path()
                                                                       : private_stream_path()}},
                  handler_} {
        handler_.attach(client_, key_);
    }

    void start() { client_.start(); }

    void stop() { client_.stop(); }

    [[nodiscard]] bool connected() const noexcept { return client_.connected(); }

    void set_key(std::string_view key) { key_.assign(key); }

    // The socket subscribes with whatever key it holds AT on_open, so a fresh
    // key only takes effect on a fresh connection.
    void reconnect() {
        client_.stop();
        client_.start();
    }

    // A TEXT ping, not a websocket control frame: XT counts only application
    // messages toward the idle timer this is holding off.
    void keepalive(UnixNanos now) noexcept {
        if (now.ns < next_ping_ns_) {
            return;
        }
        if (client_.connected()) {
            (void)client_.send(PrivateProtocol::ping_frame());
        }
        next_ping_ns_ = now.ns + k_app_ping_interval_ns;
    }

private:
    PrivateHandler handler_;
    net::WsClient client_;
    std::string key_;
    std::uint64_t next_ping_ns_{0};
};

} // namespace raikiri::venues::xt::internal
