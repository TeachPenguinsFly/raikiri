#pragma once

// the private socket: authenticates, subscribes and maps frames to sink events; sends no orders

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/venues/bybit/positions.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <memory>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::bybit::internal {

void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto topic = parse_topic(frame);
    if (!topic.has_value()) {
        return;
    }
    switch (topic->kind) {
    case TopicKind::Order: {
        static thread_local const auto storage_owner = std::make_unique<OrderUpdateBuffer>();
        auto& storage = *storage_owner;
        const auto updates = parse_order_updates(frame, storage);
        if (!updates.has_value()) {
            return;
        }
        for (const auto& wire : *updates) {
            if (const auto event = map_order_event(wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
        }
        break;
    }
    case TopicKind::Execution: {
        static thread_local const auto storage_owner = std::make_unique<ExecutionBuffer>();
        auto& storage = *storage_owner;
        const auto executions = parse_executions(frame, storage);
        if (!executions.has_value()) {
            return;
        }
        for (const auto& wire : *executions) {
            // settlements arrive here live; the rest ledger pull is the check
            if (const auto charge = map_funding_charge(wire, state.instruments, received);
                charge.has_value()) {
                state.sink->on_funding_charge(*charge);
                continue;
            }
            if (const auto event = map_execution(wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
        }
        break;
    }
    default:
        break;
    }
}

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto auth = state_->protocol.auth_frame(state_->signer, state_->venue_now());
        if (!auth.has_value() || !client_->send(*auth)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            return;
        }
        const std::array<std::string_view, 3> topics{"order", "execution", "position"};
        if (const auto frame = state_->protocol.subscribe_frame(topics); frame.has_value()) {
            (void)client_->send(frame->text);
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

// owns the socket so the handler cannot outlive its client
class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{host(state.testnet)},
                                             .port = "443",
                                             .target = std::string{private_stream_path()}},
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

} // namespace raikiri::venues::bybit::internal
