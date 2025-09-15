#pragma once

// The private socket, and only that.
//
// It subscribes and turns each frame into an event on the sink. It sends no
// orders: an order goes out over REST and is ANSWERED here, and joining those
// two halves is the execution client's job, not this one's.

#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include "../state.hpp"

namespace raikiri::venues::kucoin::detail {

class PrivateStream final : public net::WsHandler, public Pumped {
public:
    // The socket cannot mint its own ticket: it comes from a signed REST call
    // and it expires, so every reopen has to ask for a new one. Handed in as a
    // callable so this class still knows nothing but the socket.
    using Minter = std::function<std::expected<PrivateTicket, exec::VenueError>()>;

    PrivateStream(VenueState& state, MarketType market, std::uint32_t stream_id, Minter mint)
        : state_{&state}, market_{market}, stream_id_{stream_id}, mint_{std::move(mint)} {
        state_->pumps.push_back(this);
    }

    ~PrivateStream() { std::erase(state_->pumps, this); }

    void start() {
        started_ = true;
        open_stream();
    }

    void stop() {
        if (client_) {
            client_->stop();
            client_.reset();
        }
        started_ = false;
        reopen_due_ = false;
    }

    [[nodiscard]] bool connected() const noexcept {
        return client_ != nullptr && client_->connected();
    }

    void pump(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        if (reopen_due_ && state_->clock->mono().ns >= reopen_at_ns_) {
            reopen_due_ = false;
            open_stream();
        }
        if (connected() && now.ns >= next_ping_ns_) {
            if (const auto frame = protocol_.ping_frame(); !frame.empty()) {
                (void)client_->send(frame);
            }
            next_ping_ns_ = now.ns + ping_interval_ns_;
        }
    }

    void on_open() override {
        const auto topic = private_order_topic(market_);
        const auto frame = protocol_.private_subscribe_frame(topic);
        if (!frame.has_value() || !client_->send(*frame)) {
            // A socket that carries no order topic reports nothing at all,
            // which is indistinguishable from a quiet account.
            schedule_reopen();
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Connected,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_private_event(payload);
        if (!event.has_value()) {
            return; // unknown shapes are ignored, never fatal
        }
        switch (event->kind) {
        case EventKind::OrderChange: {
            const auto wire = parse_order_change(payload);
            if (!wire.has_value()) {
                return;
            }
            if (const auto out = map_order_change(*wire, state_->instruments, received);
                out.has_value()) {
                state_->sink->on_order_event(*out);
            }
            break;
        }
        case EventKind::Error: {
            if (const auto err = parse_stream_error(payload);
                err.has_value() && is_expired_token(err->code)) {
                schedule_reopen(); // the socket's own backoff would reuse the dead token
            }
            break;
        }
        case EventKind::Bbo:
        case EventKind::Trade:
        case EventKind::Depth:
        case EventKind::MarkPrice:
        case EventKind::FundingRate:
        case EventKind::Welcome:
        case EventKind::Ack:
        case EventKind::Pong:
        case EventKind::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason reason) override {
        if (reason != net::WsCloseReason::Requested) {
            // The token is in the URL and expires, so the reconnect has to
            // start from a fresh bullet rather than from the same target.
            schedule_reopen();
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

private:
    void schedule_reopen() noexcept {
        reopen_due_ = true;
        reopen_at_ns_ = state_->clock->mono().ns + k_reopen_delay_ns;
    }

    void open_stream() noexcept {
        const auto ticket = mint_();
        if (!ticket.has_value()) {
            if (ticket.error().cls == exec::ErrorClass::AuthFailure) {
                // Retrying a refused key at full speed is how an account earns
                // a lockout, so this one failure does not reschedule.
                report_auth_failure();
                return;
            }
            schedule_reopen();
            return;
        }
        if (ticket->ping_interval_ms != 0) {
            ping_interval_ns_ = ticket->ping_interval_ms * 1'000'000ULL;
        }
        next_ping_ns_ = 0;

        if (client_) {
            client_->stop();
        }
        client_ = std::make_unique<net::WsClient>(state_->io,
                                                  net::WsConfig{.host = ticket->endpoint.host,
                                                                .port = ticket->endpoint.port,
                                                                .target = ticket->endpoint.target},
                                                  *this);
        client_->start();
    }

    void report_auth_failure() noexcept {
        state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    MarketType market_;
    std::uint32_t stream_id_;
    Minter mint_;
    Protocol protocol_; // its own, so a private frame never races a public one
    std::unique_ptr<net::WsClient> client_;
    std::uint64_t ping_interval_ns_{k_default_ping_ms * 1'000'000ULL};
    std::uint64_t next_ping_ns_{0};
    std::uint64_t reopen_at_ns_{0};
    bool reopen_due_{false};
    bool started_{false};
};

} // namespace raikiri::venues::kucoin::detail
