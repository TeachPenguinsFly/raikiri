#include <raikiri/net/ws_client.hpp>

#include <raikiri/net/trust_store.hpp>

#include <raikiri/net/io_context.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <openssl/ssl.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace raikiri::net {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;
namespace websock = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

using TlsStream = beast::ssl_stream<beast::tcp_stream>;
using WsStream = websock::stream<TlsStream>;

// A link that stayed up this long was not a fluke, so the next outage starts
// its backoff from the floor instead of inheriting the previous storm's
// ceiling. Without this a venue that flaps once an hour ends up permanently
// reconnecting at reconnect_max_ms.
constexpr std::uint64_t k_stable_connection_ns = 30'000'000'000ULL;

constexpr std::string_view k_user_agent = "raikiri/0.1";

// RFC 6066 forbids IP literals in SNI and OpenSSL refuses to set one, so the
// extension is skipped for them. Venues are always DNS names; only loopback
// test servers take this branch.
[[nodiscard]] bool is_ip_literal(const std::string& host) noexcept {
    boost::system::error_code ec;
    static_cast<void>(asio::ip::make_address(host, ec));
    return !ec;
}

} // namespace

std::string_view to_string(WsCloseReason reason) noexcept {
    switch (reason) {
    case WsCloseReason::Requested:
        return "requested";
    case WsCloseReason::Transport:
        return "transport";
    case WsCloseReason::PongTimeout:
        return "pong_timeout";
    case WsCloseReason::ProtocolError:
        return "protocol_error";
    case WsCloseReason::ServerClosed:
        return "server_closed";
    }
    return "unknown";
}

namespace detail {

// Lifetime, stated once because every handler below depends on it.
//
// Two guards, for two different hazards:
//
//  1. shared_ptr<WsSession> in every completion handler. WsClient's destructor
//     drops its reference while Asio may still hold queued completions; those
//     handlers keep the session alive until the reactor drains them (or until
//     the io_context is destroyed, which destroys them). No use-after-free.
//     A `dead_` flag makes every one of them a no-op, because the WsHandler
//     reference they would call back into belongs to the caller and may well
//     have died with the client.
//
//  2. A generation counter plus a shared_ptr<Conn> per attempt. Closing a
//     socket does not retract handlers already queued for it, and a write
//     issued on a dying socket still points at the buffer it was handed. So
//     each connection attempt gets a fresh Conn that owns its own read and
//     write buffers; abandoning a connection bumps the generation, and stale
//     handlers recognise themselves and return. The Conn dies with the last
//     handler that referenced it, never while the kernel still has its bytes.
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(asio::io_context& ioc, WsConfig config, WsHandler& handler)
        : ioc_{ioc},
          cfg_{std::move(config)},
          handler_{handler},
          tls_{ssl::context::tls_client},
          resolver_{ioc},
          ping_timer_{ioc},
          reconnect_timer_{ioc},
          backoff_ms_{cfg_.reconnect_min_ms},
          rng_{static_cast<std::minstd_rand::result_type>(
              reinterpret_cast<std::uintptr_t>(this) | 1U)} {
        if (cfg_.verify_tls) {
            // The trust store is resolved explicitly rather than trusting
            // OpenSSL's compiled-in path: for a package-manager build that
            // path belongs to the build machine and does not exist here, and
            // the resulting handshake failure names the PEER, which reads
            // like the venue's problem rather than ours. Failure to find one
            // surfaces as a handshake failure, not an exception out of a
            // constructor.
            const auto trust = resolve_trust_store();
            boost::system::error_code ec;
            if (!trust.file.empty()) {
                tls_.load_verify_file(trust.file, ec);
            }
            if (!trust.dir.empty()) {
                tls_.add_verify_path(trust.dir, ec);
            }
            if (!trust.found) {
                tls_.set_default_verify_paths(ec);
            }
            tls_.set_verify_mode(ssl::verify_peer);
            tls_.set_verify_callback(ssl::host_name_verification{cfg_.host});
        } else {
            tls_.set_verify_mode(ssl::verify_none);
        }
        // Venues reject a Host header carrying the default port; test servers
        // on ephemeral ports need it.
        host_header_ = cfg_.port == "443" ? cfg_.host : cfg_.host + ":" + cfg_.port;
    }

    void start() {
        if (dead_ || running_) {
            return;
        }
        running_ = true;
        backoff_ms_ = cfg_.reconnect_min_ms;
        begin_connect();
    }

    void stop() {
        if (dead_) {
            return;
        }
        running_ = false;
        reconnect_timer_.cancel();
        if (state_ == State::Idle) {
            // Idempotent: a second stop() must not synthesize a second
            // on_close. Bumping the generation still disarms a reconnect
            // handler that expired in the same poll as the cancel.
            ++generation_;
            return;
        }
        close_with(WsCloseReason::Requested);
    }

    // Destructor path only: no callback may fire from here, because the
    // handler reference is the caller's and the caller is going away.
    void shutdown() noexcept {
        dead_ = true;
        running_ = false;
        state_ = State::Idle;
        ++generation_;
        resolver_.cancel();
        ping_timer_.cancel();
        reconnect_timer_.cancel();
        if (conn_) {
            beast::get_lowest_layer(conn_->stream).close();
            conn_.reset();
        }
        send_queue_.clear();
    }

    [[nodiscard]] bool connected() const noexcept { return state_ == State::Open; }

    [[nodiscard]] bool send(std::string_view text) {
        if (dead_) {
            return false;
        }
        if (send_queue_.size() >= cfg_.send_queue_capacity) {
            ++stats_.send_queue_full;
            return false;
        }
        send_queue_.emplace_back(text);
        drain_writes();
        return true;
    }

    [[nodiscard]] WsClient::Stats stats() const noexcept { return stats_; }

private:
    enum class State : std::uint8_t {
        Idle,
        Resolving,
        Connecting,
        TlsHandshake,
        WsHandshake,
        Open,
    };

    // One per connection attempt. Owns everything the kernel might still be
    // reading from or writing into, so abandoning it is always safe.
    struct Conn {
        Conn(asio::io_context& ioc, ssl::context& tls, std::size_t max_bytes)
            : stream{ioc.get_executor(), tls}, buffer{max_bytes} {}

        WsStream stream;
        beast::flat_buffer buffer;
        std::string write_buf;
    };

    [[nodiscard]] bool current(std::uint64_t gen, const std::shared_ptr<Conn>& conn) const noexcept {
        return !dead_ && gen == generation_ && conn == conn_;
    }

    void begin_connect() {
        ++generation_;
        const auto gen = generation_;
        conn_ = std::make_shared<Conn>(ioc_, tls_, cfg_.max_message_bytes);
        state_ = State::Resolving;
        resolver_.async_resolve(
            cfg_.host, cfg_.port,
            [self = shared_from_this(), gen, conn = conn_](const beast::error_code& ec,
                                                           const tcp::resolver::results_type& results) {
                self->on_resolve(gen, conn, ec, results);
            });
    }

    void on_resolve(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec,
                    const tcp::resolver::results_type& results) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport);
            return;
        }
        state_ = State::Connecting;
        // WsConfig carries no connect deadline. pong_timeout_ms is its only
        // "how long do we wait for this venue to answer" knob, so it doubles
        // as the setup deadline; a venue that cannot complete a handshake in
        // the time it is allowed to answer a ping is down.
        auto& lowest = beast::get_lowest_layer(conn->stream);
        lowest.expires_after(std::chrono::milliseconds{cfg_.pong_timeout_ms});
        lowest.async_connect(results, [self = shared_from_this(), gen, conn](
                                          const beast::error_code& connect_ec, const tcp::endpoint&) {
            self->on_connect(gen, conn, connect_ec);
        });
    }

    void on_connect(std::uint64_t gen, const std::shared_ptr<Conn>& conn,
                    const beast::error_code& ec) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport);
            return;
        }
        state_ = State::TlsHandshake;
        // Without SNI the venue's edge cannot pick a certificate and aborts
        // the handshake — the single most common cause of a client that works
        // against a test server and dies against production.
        if (!is_ip_literal(cfg_.host) &&
            SSL_set_tlsext_host_name(conn->stream.next_layer().native_handle(),
                                     cfg_.host.c_str()) != 1) {
            close_with(WsCloseReason::Transport);
            return;
        }
        conn->stream.next_layer().async_handshake(
            ssl::stream_base::client,
            [self = shared_from_this(), gen, conn](const beast::error_code& tls_ec) {
                self->on_tls(gen, conn, tls_ec);
            });
    }

    void on_tls(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport);
            return;
        }
        state_ = State::WsHandshake;
        // Hand the deadline over to beast's websocket timer: the tcp_stream
        // timer and the websocket timer fight if both are armed.
        beast::get_lowest_layer(conn->stream).expires_never();

        websock::stream_base::timeout timeout{};
        timeout.handshake_timeout = std::chrono::milliseconds{cfg_.pong_timeout_ms};
        // Our own ping cadence drives liveness (see on_ping_timer). Beast's
        // idle timer would run a second, uncoordinated one and close the
        // socket out from under the state machine.
        timeout.idle_timeout = websock::stream_base::none();
        timeout.keep_alive_pings = false;
        conn->stream.set_option(timeout);

        conn->stream.read_message_max(cfg_.max_message_bytes);
        conn->stream.text(true);
        conn->stream.set_option(websock::stream_base::decorator([](websock::request_type& req) {
            req.set(beast::http::field::user_agent, k_user_agent);
        }));
        // Raw `this` is safe: the callback only runs inside a read on this
        // stream, and that read's handler holds a shared_ptr to the session.
        // Capturing shared_from_this() here would instead create a cycle,
        // since the stream that owns the callback is owned by the session.
        conn->stream.control_callback([this](websock::frame_type kind, beast::string_view) {
            if (kind == websock::frame_type::pong) {
                awaiting_pong_ = false;
            }
        });

        conn->stream.async_handshake(
            host_header_, cfg_.target,
            [self = shared_from_this(), gen, conn](const beast::error_code& ws_ec) {
                self->on_ws_handshake(gen, conn, ws_ec);
            });
    }

    void on_ws_handshake(std::uint64_t gen, const std::shared_ptr<Conn>& conn,
                         const beast::error_code& ec) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport);
            return;
        }
        state_ = State::Open;
        opened_at_ = clock_.mono();
        awaiting_pong_ = false;
        writing_ = false;
        ping_pending_ = false;

        // on_open before the read loop: the adapter re-asserts subscriptions
        // from here, and it must be able to send() while it does.
        try {
            handler_.on_open();
        } catch (...) {
            // A handler that cannot replay its subscriptions has nothing to
            // do with the socket, so this is not Transport. Close and let the
            // backoff give it another try with a clean stream.
            close_with(WsCloseReason::ProtocolError);
            return;
        }
        if (!current(gen, conn)) {
            return; // on_open called stop()
        }
        drain_writes();
        arm_ping_timer(false);
        do_read(gen, conn);
    }

    void do_read(std::uint64_t gen, const std::shared_ptr<Conn>& conn) {
        conn->stream.async_read(conn->buffer, [self = shared_from_this(), gen, conn](
                                                  const beast::error_code& ec, std::size_t bytes) {
            self->on_read(gen, conn, ec, bytes);
        });
    }

    void on_read(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec,
                 std::size_t bytes) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec == websock::error::closed) {
            close_with(WsCloseReason::ServerClosed);
            return;
        }
        if (ec == websock::error::message_too_big || ec == websock::error::buffer_overflow) {
            close_with(WsCloseReason::ProtocolError);
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport);
            return;
        }
        // Any inbound frame proves the peer is alive, not just a pong. Venues
        // under load answer pings late but keep streaming book updates, and
        // dropping those connections is self-inflicted downtime.
        awaiting_pong_ = false;
        if (bytes > cfg_.max_message_bytes) {
            close_with(WsCloseReason::ProtocolError);
            return;
        }
        ++stats_.messages_received;

        // flat_buffer is contiguous, so the handler gets one view with no
        // copy. It is invalidated by the consume() below, which is exactly
        // the contract on_message publishes.
        const auto data = conn->buffer.data();
        handler_.on_message(std::string_view{static_cast<const char*>(data.data()), data.size()},
                            clock_.wall());
        conn->buffer.consume(conn->buffer.size());
        do_read(gen, conn);
    }

    // Beast forbids overlapping writes, and async_ping is a write. One flag
    // serialises both, with the ping jumping the queue because liveness must
    // not sit behind a backlog that is itself the symptom.
    void drain_writes() {
        if (dead_ || state_ != State::Open || writing_ || !conn_) {
            return;
        }
        const auto gen = generation_;
        auto conn = conn_;
        if (ping_pending_) {
            ping_pending_ = false;
            writing_ = true;
            conn->stream.async_ping(websock::ping_data{},
                                    [self = shared_from_this(), gen, conn](const beast::error_code& ec) {
                                        self->on_write(gen, conn, ec, true);
                                    });
            return;
        }
        if (send_queue_.empty()) {
            return;
        }
        // The payload moves into the Conn so that a write left in flight on an
        // abandoned socket cannot outlive the buffer it points at. The price
        // is that a message in flight when a connection dies is lost, which is
        // the right trade: on_open replays subscriptions anyway, and a stale
        // request answered after a reconnect is worse than one never sent.
        conn->write_buf = std::move(send_queue_.front());
        send_queue_.pop_front();
        writing_ = true;
        conn->stream.async_write(asio::buffer(conn->write_buf),
                                 [self = shared_from_this(), gen, conn](const beast::error_code& ec,
                                                                        std::size_t) {
                                     self->on_write(gen, conn, ec, false);
                                 });
    }

    void on_write(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec,
                  bool was_ping) {
        if (!current(gen, conn)) {
            return;
        }
        writing_ = false;
        if (ec) {
            close_with(WsCloseReason::Transport);
            return;
        }
        if (!was_ping) {
            ++stats_.messages_sent;
        }
        drain_writes();
    }

    // Two-phase on one timer: fire at ping_interval to send, fire again at
    // pong_timeout to judge. A single interval timer would only notice a dead
    // peer one full interval late, which for a 20s cadence is 20s of stale
    // book being treated as live.
    void arm_ping_timer(bool awaiting) {
        const auto delay = awaiting ? cfg_.pong_timeout_ms : cfg_.ping_interval_ms;
        ping_timer_.expires_after(std::chrono::milliseconds{delay});
        ping_timer_.async_wait(
            [self = shared_from_this(), gen = generation_, awaiting](const beast::error_code& ec) {
                self->on_ping_timer(gen, ec, awaiting);
            });
    }

    void on_ping_timer(std::uint64_t gen, const beast::error_code& ec, bool was_awaiting) {
        if (dead_ || ec || gen != generation_ || state_ != State::Open) {
            return;
        }
        if (was_awaiting) {
            if (awaiting_pong_) {
                ++stats_.pong_timeouts;
                close_with(WsCloseReason::PongTimeout);
                return;
            }
            arm_ping_timer(false);
            return;
        }
        awaiting_pong_ = true;
        ping_pending_ = true;
        drain_writes();
        arm_ping_timer(true);
    }

    void close_with(WsCloseReason reason) {
        if (state_ == State::Idle) {
            return;
        }
        const bool was_stable = state_ == State::Open &&
                                clock_.mono().ns - opened_at_.ns > k_stable_connection_ns;
        state_ = State::Idle;
        ++generation_;
        writing_ = false;
        ping_pending_ = false;
        ping_timer_.cancel();
        if (conn_) {
            // Hard close, not a websocket close handshake: a peer that just
            // failed us has no reason to answer one, and the graceful path
            // would park the reconnect behind a timeout we already paid once.
            beast::get_lowest_layer(conn_->stream).close();
            conn_.reset();
        }
        if (reason == WsCloseReason::Requested) {
            running_ = false;
            reconnect_timer_.cancel();
        }
        if (!dead_) {
            try {
                handler_.on_close(reason);
            } catch (...) {
                // A handler that throws out of on_close must not take the
                // reconnect down with it.
            }
        }
        if (running_ && !dead_) {
            schedule_reconnect(was_stable);
        }
    }

    void schedule_reconnect(bool reset_backoff) {
        if (reset_backoff) {
            backoff_ms_ = cfg_.reconnect_min_ms;
        }
        const std::uint32_t base = backoff_ms_;
        backoff_ms_ = base >= cfg_.reconnect_max_ms / 2 ? cfg_.reconnect_max_ms : base * 2;

        // Jitter over a quarter of the window. Eight adapters that lost the
        // same venue must not come back in lockstep and hand it a thundering
        // herd, but the cadence still has to be legible to an operator
        // reading logs, which full jitter is not.
        const std::uint32_t spread = base / 4 + 1;
        const auto jitter = static_cast<std::uint32_t>(rng_() % spread);

        // Counted on the attempt, not the success: an adapter watching this
        // number wants to see churn even while the venue is refusing us.
        ++stats_.reconnects;
        reconnect_timer_.expires_after(std::chrono::milliseconds{base + jitter});
        reconnect_timer_.async_wait(
            [self = shared_from_this(), gen = generation_](const beast::error_code& ec) {
                if (self->dead_ || ec || !self->running_ || gen != self->generation_) {
                    return;
                }
                self->begin_connect();
            });
    }

    asio::io_context& ioc_;
    WsConfig cfg_;
    WsHandler& handler_;
    ssl::context tls_;
    tcp::resolver resolver_;
    asio::steady_timer ping_timer_;
    asio::steady_timer reconnect_timer_;
    LiveClock clock_;

    std::shared_ptr<Conn> conn_;
    std::deque<std::string> send_queue_;
    std::string host_header_;
    WsClient::Stats stats_;
    MonoNanos opened_at_{};
    std::uint64_t generation_{0};
    std::uint32_t backoff_ms_;
    std::minstd_rand rng_;
    State state_{State::Idle};
    bool running_{false};
    bool dead_{false};
    bool writing_{false};
    bool ping_pending_{false};
    bool awaiting_pong_{false};
};

} // namespace detail

struct WsClient::Impl {
    Impl(asio::io_context& ioc, WsConfig config, WsHandler& handler)
        : session{std::make_shared<detail::WsSession>(ioc, std::move(config), handler)} {}

    std::shared_ptr<detail::WsSession> session;
};

WsClient::WsClient(IoContext& io, WsConfig config, WsHandler& handler)
    : impl_{std::make_unique<Impl>(*static_cast<asio::io_context*>(io.native()), std::move(config),
                                   handler)} {}

WsClient::~WsClient() {
    impl_->session->shutdown();
}

void WsClient::start() {
    impl_->session->start();
}

void WsClient::stop() {
    impl_->session->stop();
}

bool WsClient::connected() const noexcept {
    return impl_->session->connected();
}

bool WsClient::send(std::string_view text) {
    return impl_->session->send(text);
}

WsClient::Stats WsClient::stats() const noexcept {
    return impl_->session->stats();
}

} // namespace raikiri::net
