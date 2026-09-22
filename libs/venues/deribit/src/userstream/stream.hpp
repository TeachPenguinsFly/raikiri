#pragma once

// The private socket, and only that.
//
// It authenticates, subscribes and hands each frame to the shared JSON-RPC
// reader in frames.cpp — the dialect is the same on both sockets, which is why
// that reader belongs to neither. It sends no orders: an order goes out over
// REST and is ANSWERED here, and joining those two halves is the execution
// client's job, not this one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <array>
#include <span>
#include <string>

#include "../state.hpp"

namespace raikiri::venues::deribit::internal {

// Distinguishes the private socket's connectivity from the market-data one in
// everything the gateway logs.
constexpr std::uint64_t k_private_stream_id = 1;

// The private socket. Deribit authenticates the CONNECTION rather than each
// message: public/auth once, and every private call afterwards travels without
// a token. The subscription that matters is only legal once that succeeded,
// which is why it is sent from the auth reply and nowhere else.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        (void)client_->send(
            state_->private_protocol.set_heartbeat_frame(k_heartbeat_interval_s).text);
        const auto frame = state_->private_protocol.auth_frame(state_->signer, state_->venue_now());
        if (!frame.has_value() || !client_->send(frame->text)) {
            fail();
            return;
        }
        auth_id_ = frame->id;
        authenticated_ = false;
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return;
        }
        if (event->kind == EventKind::Response) {
            const auto response = parse_response(payload);
            if (response.has_value() && response->id == auth_id_) {
                on_auth(*response);
                return;
            }
        }
        if (handle_frame(*state_, payload, received)) {
            // An unanswered test_request costs the connection, and this socket
            // answers on its own id sequence.
            (void)client_->send(state_->private_protocol.test_frame().text);
        }
    }

    void on_close(net::WsCloseReason) override {
        authenticated_ = false;
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }

private:
    void fail() {
        state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_auth(const WireResponse& response) {
        if (response.failed()) {
            state_->last_error_code = response.error_code;
            fail();
            return;
        }
        authenticated_ = true;
        // One channel per (kind, currency): the whole account in four, and a
        // fill on an instrument nothing subscribed to still arrives.
        std::array<std::array<char, k_max_channel_len>, 4> storage{};
        std::array<Subscription, 4> channels{};
        std::size_t count = 0;
        for (const auto market : {MarketType::Perp, MarketType::Option}) {
            for (const auto currency : k_currencies) {
                const auto channel =
                    user_changes_channel(kind_of(market), currency, storage[count]);
                if (channel.empty()) {
                    fail();
                    return;
                }
                channels[count] = Subscription{.channel = channel};
                ++count;
            }
        }
        const auto frame =
            state_->private_protocol.private_subscribe_frame(std::span{channels}.first(count));
        if (!frame.has_value() || !client_->send(frame->text)) {
            fail();
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    std::int64_t auth_id_{0};
    bool authenticated_{false};
};

// The socket itself, owned as one thing so a caller cannot hold the handler
// without the client it is attached to.
class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{stream_host(state.testnet)},
                                             .port = std::string{stream_port()},
                                             .target = std::string{stream_path()}},
                               handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    // A socket that is up but not yet through public/auth carries no private
    // channel at all, so being open is not the same as being usable.
    [[nodiscard]] bool connected() const noexcept {
        return ws_.connected() && handler_.authenticated();
    }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

} // namespace raikiri::venues::deribit::internal
