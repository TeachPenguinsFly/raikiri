#pragma once

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/spot.hpp>

#include <string>

#include "../state.hpp"

namespace raikiri::venues::weex::internal {

// orders are never sent here, so a reconnect has nothing to resend; gaps close by reconciliation
class SpotPrivateHandler final : public net::WsHandler {
public:
    explicit SpotPrivateHandler(VenueState& state) : state_{&state} {}

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    void on_open() override {
        for (const auto frame : k_spot_private_subscribes) {
            if (client_ == nullptr || !client_->send(frame)) {
                state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                               .stream_id = k_spot_private_stream_id,
                                               .ts = state_->clock->wall()});
                return;
            }
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_spot_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        if (const auto reply = state_->spot_feed.on_frame(payload, received); reply.has_value() && client_ != nullptr) {
            (void)client_->send(*reply);
        }
    }

    void on_close(net::WsCloseReason) override { state_->spot_feed.on_close(); }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class SpotPrivateStream {
public:
    explicit SpotPrivateStream(VenueState& state) : handler_{state}, client_{state.io, config(state), handler_} {
        handler_.attach(client_);
    }

    void start() { client_.start(); }

    void stop() { client_.stop(); }

private:
    // the handshake is signed afresh for every connect, since the timestamp lives 30 s
    static net::WsConfig config(VenueState& state) {
        net::WsConfig cfg{};
        cfg.host = std::string{ep::k_spot_stream_host};
        cfg.port = std::string{ep::k_stream_port};
        cfg.target = std::string{ep::k_spot_private_ws};
        cfg.headers_for_connect = [&state] {
            return spot_stream_headers(state.signer, state.passphrase, state.venue_now());
        };
        return cfg;
    }

    SpotPrivateHandler handler_;
    net::WsClient client_;
};

} // namespace raikiri::venues::weex::internal
