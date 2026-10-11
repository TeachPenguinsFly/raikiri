#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/trust_store.hpp>
#include <raikiri/net/ws_client.hpp>

#include <openssl/ssl.h>

#include "sni.hpp"

#ifdef __linux__
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <sys/socket.h>

#include <array>
#include <boost/asio/associated_immediate_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/dispatch.hpp>
#include <cerrno>
#include <cstring>
#endif

#include <algorithm>
#include <boost/asio/bind_immediate_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
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

// the concrete executor type; any_io_executor is rebuilt at each read step
using Executor = asio::io_context::executor_type;
#ifdef __linux__
// reads under the tls layer with recvmsg to get the SO_TIMESTAMPING software rx stamp
class StampedSocket : public asio::basic_stream_socket<tcp, Executor> {
public:
    using asio::basic_stream_socket<tcp, Executor>::basic_stream_socket;

    void enable_stamps() noexcept {
        const int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        stamps_ = ::setsockopt(native_handle(), SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof flags) == 0;
    }

    [[nodiscard]] std::uint64_t last_arrival_ns() const noexcept { return last_arrival_ns_; }

    template <typename MutableBuffers, typename Handler>
    auto async_read_some(const MutableBuffers& buffers, Handler&& handler) {
        return asio::async_initiate<Handler, void(boost::system::error_code, std::size_t)>(
            [self = this](auto done, const MutableBuffers& into) { self->start(std::move(done), into); },
            handler,
            buffers);
    }

private:
    template <typename Done, typename MutableBuffers> void start(Done done, const MutableBuffers& into) {
        boost::system::error_code ec;
        std::size_t got = 0;
        // ssl completes from its own buffer with a zero-byte read, which recvmsg turns into eof or a wait
        if (asio::buffer_size(into) == 0 || attempt(into, ec, got)) {
            const auto ex = asio::get_associated_immediate_executor(done, get_executor());
            asio::dispatch(ex, [done = std::move(done), ec, got]() mutable { std::move(done)(ec, got); });
            return;
        }
        async_wait(asio::socket_base::wait_read,
                   [this, done = std::move(done), into](const boost::system::error_code& wait_ec) mutable {
                       if (wait_ec) {
                           std::move(done)(wait_ec, 0);
                           return;
                       }
                       start(std::move(done), into);
                   });
    }

    // false when the socket is empty and the caller must wait
    template <typename MutableBuffers>
    bool attempt(const MutableBuffers& into, boost::system::error_code& ec, std::size_t& got) {
        std::array<::iovec, 4> iov{};
        std::size_t count = 0;
        for (auto it = asio::buffer_sequence_begin(into); it != asio::buffer_sequence_end(into) && count < iov.size();
             ++it) {
            const asio::mutable_buffer piece{*it};
            iov[count++] = ::iovec{.iov_base = piece.data(), .iov_len = piece.size()};
        }
        alignas(::cmsghdr) std::array<char, CMSG_SPACE(sizeof(::scm_timestamping))> control{};
        ::msghdr msg{};
        msg.msg_iov = iov.data();
        msg.msg_iovlen = count;
        msg.msg_control = stamps_ ? control.data() : nullptr;
        msg.msg_controllen = stamps_ ? control.size() : 0;
        for (;;) {
            const auto n = ::recvmsg(native_handle(), &msg, MSG_DONTWAIT);
            if (n > 0) {
                got = static_cast<std::size_t>(n);
                // a read the kernel did not stamp must not inherit an earlier read's stamp; zero falls back to the wall
                // clock
                last_arrival_ns_ = 0;
                for (auto* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
                    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING) {
                        ::scm_timestamping stamp{};
                        std::memcpy(&stamp, CMSG_DATA(c), sizeof stamp);
                        last_arrival_ns_ = static_cast<std::uint64_t>(stamp.ts[0].tv_sec) * 1'000'000'000ULL +
                                           static_cast<std::uint64_t>(stamp.ts[0].tv_nsec);
                    }
                }
                return true;
            }
            if (n == 0) {
                ec = asio::error::eof;
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            ec = boost::system::error_code{errno, boost::system::system_category()};
            return true;
        }
    }

    std::uint64_t last_arrival_ns_{0};
    bool stamps_{false};
};

// closing is the plain socket's job; beast finds these by adl
[[maybe_unused]] void teardown(beast::role_type role, StampedSocket& socket, beast::error_code& ec) {
    websock::teardown(role, static_cast<asio::basic_stream_socket<tcp, Executor>&>(socket), ec);
}

template <typename Handler> void async_teardown(beast::role_type role, StampedSocket& socket, Handler&& handler) {
    websock::async_teardown(
        role, static_cast<asio::basic_stream_socket<tcp, Executor>&>(socket), std::forward<Handler>(handler));
}

using Socket = StampedSocket;
#else
using Socket = asio::basic_stream_socket<tcp, Executor>;
#endif
using TlsStream = beast::ssl_stream<Socket>;
using WsStream = websock::stream<TlsStream>;
using Resolver = asio::ip::basic_resolver<tcp, Executor>;
using Timer = asio::
    basic_waitable_timer<std::chrono::steady_clock, asio::wait_traits<std::chrono::steady_clock>, Executor>;

// inline reads before one posts, keeping recursion shallow and letting other sockets run
constexpr std::uint32_t k_inline_reads = 8;

// extended refuses a request or handshake without one (403)
constexpr std::string_view k_user_agent = "raikiri/0.1";

// rfc 6066 forbids ip literals in sni; only loopback test servers hit this
[[nodiscard]] bool is_ip_literal(const std::string& host) noexcept {
    boost::system::error_code ec;
    static_cast<void>(asio::ip::make_address(host, ec));
    return !ec;
}

[[nodiscard]] std::string describe(std::string_view stage, const beast::error_code& ec) {
    return std::format("{}: {} {} {}", stage, ec.category().name(), ec.value(), ec.message());
}

[[nodiscard]] std::string describe(const websock::close_reason& frame) {
    if (frame.code == websock::close_code::none) {
        return "close frame without a code";
    }
    if (frame.reason.empty()) {
        return std::format("close {}", static_cast<unsigned>(frame.code));
    }
    return std::format("close {} {}", static_cast<unsigned>(frame.code), frame.reason.c_str());
}

PongObserver g_pong_observer;       // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
ArrivalObserver g_arrival_observer; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

} // namespace

void set_pong_observer(PongObserver observer) {
    g_pong_observer = std::move(observer);
}

void set_arrival_observer(ArrivalObserver observer) {
    g_arrival_observer = std::move(observer);
}

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

namespace internal {

// handlers hold a shared_ptr to the session and `dead_` makes them no-ops after the client dies
// each attempt gets a fresh Conn and generation, so stale handlers return
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(asio::io_context& ioc, WsConfig config, WsHandler& handler)
        : ioc_{ioc}, cfg_{std::move(config)}, handler_{handler}, tls_{ssl::context::tls_client}, resolver_{ioc},
          ping_timer_{ioc}, reconnect_timer_{ioc}, backoff_{host_backoff(cfg_.host, cfg_.port, cfg_.backoff)} {
        if (cfg_.verify_tls) {
            const auto trust = resolve_trust_store();
            boost::system::error_code ec;
            if (!trust.file.empty()) {
                (void)tls_.load_verify_file(trust.file, ec);
            }
            if (!trust.dir.empty()) {
                (void)tls_.add_verify_path(trust.dir, ec);
            }
            if (!trust.found) {
                (void)tls_.set_default_verify_paths(ec);
            }
            tls_.set_verify_mode(ssl::verify_peer);
            tls_.set_verify_callback(ssl::host_name_verification{cfg_.host});
        } else {
            tls_.set_verify_mode(ssl::verify_none);
        }
        host_header_ = cfg_.port == "443" ? cfg_.host : cfg_.host + ":" + cfg_.port;
    }

    void start() {
        if (dead_ || running_) {
            return;
        }
        running_ = true;
        admit_connect();
    }

    void stop() {
        if (dead_) {
            return;
        }
        running_ = false;
        reconnect_timer_.cancel();
        if (state_ == State::Idle) {
            // idempotent: a second stop() must not synthesize a second on_close
            ++generation_;
            abandon();
            return;
        }
        close_with(WsCloseReason::Requested);
    }

    void shutdown() noexcept {
        abandon();
        dead_ = true;
        running_ = false;
        state_ = State::Idle;
        ++generation_;
        resolver_.cancel();
        ping_timer_.cancel();
        reconnect_timer_.cancel();
        if (conn_) {
            close_socket(*conn_);
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

    [[nodiscard]] std::string_view close_detail() const noexcept { return close_detail_; }

private:
    enum class State : std::uint8_t {
        Idle,
        Resolving,
        Connecting,
        TlsHandshake,
        WsHandshake,
        Open,
    };

    struct Conn {
        Conn(asio::io_context& ioc, ssl::context& tls, std::size_t max_bytes)
            : stream{ioc.get_executor(), tls}, buffer{max_bytes} {}

        WsStream stream;
        beast::flat_buffer buffer;
        std::string write_buf;
        websock::response_type response;
    };

    // reused by on_read so a read copies no shared_ptr
    struct ReadHandler {
        std::shared_ptr<WsSession> self;
        std::shared_ptr<Conn> conn;
        std::uint64_t gen;

        void operator()(const beast::error_code& ec, std::size_t bytes) {
            auto* session = self.get();
            session->on_read(std::move(*this), ec, bytes);
        }
    };

    static void close_socket(Conn& conn) noexcept {
        beast::error_code ignored;
        static_cast<void>(beast::get_lowest_layer(conn.stream).close(ignored));
    }

    [[nodiscard]] bool current(std::uint64_t gen, const std::shared_ptr<Conn>& conn) const noexcept {
        return !dead_ && gen == generation_ && conn == conn_;
    }

    void begin_connect() {
        ++generation_;
        const auto gen = generation_;
        conn_ = std::make_shared<Conn>(ioc_, tls_, cfg_.max_message_bytes);
        state_ = State::Resolving;
        resolver_.async_resolve(cfg_.host,
                                cfg_.port,
                                [self = shared_from_this(), gen, conn = conn_](
                                    const beast::error_code& ec, const tcp::resolver::results_type& results) {
                                    self->on_resolve(gen, conn, ec, results);
                                });
    }

    void on_resolve(std::uint64_t gen,
                    const std::shared_ptr<Conn>& conn,
                    const beast::error_code& ec,
                    const tcp::resolver::results_type& results) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport, describe("resolve", ec));
            return;
        }
        state_ = State::Connecting;
        // WsConfig has no connect deadline; the idle ping timer bounds connect and tls
        ping_timer_.expires_after(std::chrono::milliseconds{cfg_.pong_timeout_ms});
        ping_timer_.async_wait([self = shared_from_this(), gen](const beast::error_code& timer_ec) {
            self->on_handshake_deadline(gen, timer_ec);
        });
        asio::async_connect(
            beast::get_lowest_layer(conn->stream),
            results,
            [self = shared_from_this(), gen, conn](const beast::error_code& connect_ec, const tcp::endpoint&) {
                self->on_connect(gen, conn, connect_ec);
            });
    }

    void on_handshake_deadline(std::uint64_t gen, const beast::error_code& ec) {
        if (dead_ || ec || gen != generation_) {
            return;
        }
        if (state_ == State::Connecting) {
            close_with(WsCloseReason::Transport, describe("connect", beast::error::timeout));
        } else if (state_ == State::TlsHandshake) {
            close_with(WsCloseReason::Transport, describe("tls handshake", beast::error::timeout));
        }
    }

    void on_connect(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport, describe("connect", ec));
            return;
        }
        state_ = State::TlsHandshake;
        boost::system::error_code nodelay_ec;
        (void)beast::get_lowest_layer(conn->stream).set_option(tcp::no_delay{true}, nodelay_ec);
#ifdef __linux__
        beast::get_lowest_layer(conn->stream).enable_stamps();
#endif
        if (!is_ip_literal(cfg_.host) &&
            !internal::set_sni_host(conn->stream.next_layer().native_handle(), cfg_.host.c_str())) {
            close_with(WsCloseReason::Transport, "sni: refused");
            return;
        }
        conn->stream.next_layer().async_handshake(
            ssl::stream_base::client, [self = shared_from_this(), gen, conn](const beast::error_code& tls_ec) {
                self->on_tls(gen, conn, tls_ec);
            });
    }

    void on_tls(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport, describe("tls handshake", ec));
            return;
        }
        state_ = State::WsHandshake;
        ping_timer_.cancel();

        websock::stream_base::timeout timeout{};
        timeout.handshake_timeout = std::chrono::milliseconds{cfg_.pong_timeout_ms};
        timeout.idle_timeout = websock::stream_base::none();
        timeout.keep_alive_pings = false;
        conn->stream.set_option(timeout);

        conn->stream.read_message_max(cfg_.max_message_bytes);
        conn->stream.text(true);
        if (cfg_.permessage_deflate) {
            websock::permessage_deflate deflate{};
            deflate.client_enable = true;
            conn->stream.set_option(deflate);
        }
        auto headers = cfg_.headers_for_connect ? cfg_.headers_for_connect()
                                                : std::vector<std::pair<std::string, std::string>>{};
        conn->stream.set_option(
            websock::stream_base::decorator([headers = std::move(headers)](websock::request_type& req) {
                req.set(beast::http::field::user_agent, k_user_agent);
                for (const auto& [name, value] : headers) {
                    req.set(name, value);
                }
            }));
        conn->stream.control_callback([this](websock::frame_type kind, beast::string_view) {
            if (kind == websock::frame_type::pong) {
                awaiting_pong_ = false;
                if (ping_sent_ != std::chrono::steady_clock::time_point{} && g_pong_observer) {
                    const auto rtt = std::chrono::steady_clock::now() - ping_sent_;
                    g_pong_observer(
                        cfg_.host,
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(rtt).count()));
                }
                ping_sent_ = {};
            }
        });

        target_ = cfg_.target_for_connect ? cfg_.target_for_connect() : cfg_.target;
        conn->stream.async_handshake(conn->response,
                                     host_header_,
                                     target_,
                                     [self = shared_from_this(), gen, conn](const beast::error_code& ws_ec) {
                                         self->on_ws_handshake(gen, conn, ws_ec);
                                     });
    }

    void on_ws_handshake(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec) {
        if (!current(gen, conn)) {
            return;
        }
        if (ec) {
            auto detail = describe("ws handshake", ec);
            if (ec == websock::error::upgrade_declined) {
                const auto status = conn->response.result_int();
                detail += std::format(" (HTTP {})", status);
                refused_ = status == 429 || status == 418;
                retry_after_ns_ = retry_after_ns(conn->response[beast::http::field::retry_after]);
            }
            close_with(WsCloseReason::Transport, std::move(detail));
            return;
        }
        state_ = State::Open;
        opened_at_ = clock_.mono();
        received_at_open_ = stats_.messages_received;
        healthy_reported_ = false;
        if (std::exchange(attempting_, false)) {
            backoff_->opened();
        }
        awaiting_pong_ = false;
        writing_ = false;
        ping_pending_ = false;

        try {
            handler_.on_open();
        } catch (...) {
            close_with(WsCloseReason::ProtocolError, "on_open threw");
            return;
        }
        if (!current(gen, conn)) {
            return; // on_open called stop()
        }
        drain_writes();
        arm_ping_timer(false);
        do_read(ReadHandler{shared_from_this(), conn, gen});
    }

    void do_read(ReadHandler handler) {
        auto& conn = *handler.conn;
        if (++reads_since_post_ < k_inline_reads) {
            conn.stream.async_read(conn.buffer, asio::bind_immediate_executor(ioc_.get_executor(), std::move(handler)));
            return;
        }
        reads_since_post_ = 0;
        conn.stream.async_read(conn.buffer, std::move(handler));
    }

    void on_read(ReadHandler handler, const beast::error_code& ec, std::size_t bytes) {
        const auto& conn = handler.conn;
        if (!current(handler.gen, conn)) {
            return;
        }
        if (ec == websock::error::closed) {
            close_with(WsCloseReason::ServerClosed, describe(conn->stream.reason()));
            return;
        }
        if (ec == websock::error::message_too_big || ec == websock::error::buffer_overflow) {
            close_with(WsCloseReason::ProtocolError, describe("read", ec));
            return;
        }
        if (ec) {
            close_with(WsCloseReason::Transport, describe("read", ec));
            return;
        }
        awaiting_pong_ = false;
        if (bytes > cfg_.max_message_bytes) {
            close_with(WsCloseReason::ProtocolError, std::format("read: {} byte message", bytes));
            return;
        }
        ++stats_.messages_received;

        const auto data = conn->buffer.data();
        auto received = clock_.wall();
#ifdef __linux__
        if (const auto arrival = beast::get_lowest_layer(conn->stream).last_arrival_ns();
            arrival != 0 && arrival <= received.ns) {
            if (g_arrival_observer) {
                g_arrival_observer(cfg_.host, received.ns - arrival);
            }
            received = UnixNanos{arrival};
        }
#endif
        handler_.on_message(std::string_view{static_cast<const char*>(data.data()), data.size()}, received);
        conn->buffer.consume(conn->buffer.size());
        do_read(std::move(handler));
    }

    // beast forbids overlapping writes, and async_ping is a write
    void drain_writes() {
        if (dead_ || state_ != State::Open || writing_ || !conn_) {
            return;
        }
        const auto gen = generation_;
        auto conn = conn_;
        if (ping_pending_) {
            ping_pending_ = false;
            writing_ = true;
            ping_sent_ = std::chrono::steady_clock::now();
            conn->stream.async_ping(websock::ping_data{},
                                    [self = shared_from_this(), gen, conn](const beast::error_code& ec) {
                                        self->on_write(gen, conn, ec, true);
                                    });
            return;
        }
        if (send_queue_.empty()) {
            return;
        }
        conn->write_buf = std::move(send_queue_.front());
        send_queue_.pop_front();
        writing_ = true;
        conn->stream.async_write(asio::buffer(conn->write_buf),
                                 [self = shared_from_this(), gen, conn](const beast::error_code& ec, std::size_t) {
                                     self->on_write(gen, conn, ec, false);
                                 });
    }

    void on_write(std::uint64_t gen, const std::shared_ptr<Conn>& conn, const beast::error_code& ec, bool was_ping) {
        if (!current(gen, conn)) {
            return;
        }
        writing_ = false;
        if (ec) {
            close_with(WsCloseReason::Transport, describe(was_ping ? "ping" : "write", ec));
            return;
        }
        if (!was_ping) {
            ++stats_.messages_sent;
        }
        drain_writes();
    }

    void arm_ping_timer(bool awaiting) {
        const auto delay = awaiting ? cfg_.pong_timeout_ms : cfg_.ping_interval_ms;
        ping_timer_.expires_after(std::chrono::milliseconds{delay});
        ping_timer_.async_wait([self = shared_from_this(), gen = generation_, awaiting](const beast::error_code& ec) {
            self->on_ping_timer(gen, ec, awaiting);
        });
    }

    void on_ping_timer(std::uint64_t gen, const beast::error_code& ec, bool was_awaiting) {
        if (dead_ || ec || gen != generation_ || state_ != State::Open) {
            return;
        }
        if (!healthy_reported_ && stats_.messages_received > received_at_open_ &&
            clock_.mono().ns - opened_at_.ns >= backoff_->policy().healthy_ns) {
            healthy_reported_ = true;
            backoff_->healthy();
        }
        if (was_awaiting) {
            if (awaiting_pong_) {
                ++stats_.pong_timeouts;
                close_with(WsCloseReason::PongTimeout, std::format("no pong within {} ms", cfg_.pong_timeout_ms));
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

    void close_with(WsCloseReason reason, std::string detail = {}) {
        if (state_ == State::Idle) {
            return;
        }
        close_detail_ = std::move(detail);
        state_ = State::Idle;
        ++generation_;
        writing_ = false;
        ping_pending_ = false;
        ping_timer_.cancel();
        if (conn_) {
            close_socket(*conn_);
            conn_.reset();
        }
        if (!cfg_.keep_queue_on_close) {
            send_queue_.clear();
        }
        if (reason == WsCloseReason::Requested) {
            running_ = false;
            reconnect_timer_.cancel();
            abandon();
        } else if (!dead_) {
            back_off();
        }
        if (!dead_) {
            try {
                handler_.on_close(reason);
            } catch (...) {
                // a throwing handler must not stop the reconnect below
            }
        }
        if (running_ && !dead_) {
            ++stats_.reconnects;
            admit_connect();
        }
    }

    void back_off() {
        attempting_ = false;
        const auto kind = std::exchange(refused_, false) ? WsFailure::Refused : WsFailure::Transport;
        const auto delay_ns = backoff_->failed(clock_.mono().ns, kind, std::exchange(retry_after_ns_, 0));
        if (delay_ns != 0 && ::raikiri::log::logger() != nullptr) {
            RLOG_WARN("ws {}: backing off {} ms after {} ({})",
                      cfg_.host,
                      delay_ns / 1'000'000,
                      kind == WsFailure::Refused ? "a refused handshake" : "a transport failure",
                      close_detail_);
        }
    }

    void abandon() noexcept {
        if (std::exchange(attempting_, false)) {
            backoff_->abandoned();
        }
    }

    void admit_connect() {
        auto wait_ns = std::uint64_t{0};
        if (!attempting_) {
            wait_ns = backoff_->admit(clock_.mono().ns);
            attempting_ = wait_ns == 0;
        }
        if (wait_ns == 0 && cfg_.connect_gate) {
            wait_ns = cfg_.connect_gate();
        }
        if (wait_ns == 0) {
            begin_connect();
            return;
        }
        reconnect_timer_.expires_after(std::chrono::nanoseconds{wait_ns});
        reconnect_timer_.async_wait([self = shared_from_this(), gen = generation_](const beast::error_code& ec) {
            if (self->dead_ || ec || !self->running_ || gen != self->generation_) {
                return;
            }
            self->admit_connect();
        });
    }

    asio::io_context& ioc_;
    WsConfig cfg_;
    WsHandler& handler_;
    ssl::context tls_;
    Resolver resolver_;
    Timer ping_timer_;
    Timer reconnect_timer_;
    LiveClock clock_;

    std::shared_ptr<Conn> conn_;
    std::deque<std::string> send_queue_;
    std::string host_header_;
    std::string target_;
    std::string close_detail_;
    WsClient::Stats stats_;
    MonoNanos opened_at_{};
    std::uint64_t generation_{0};
    std::shared_ptr<HostBackoff> backoff_;
    std::uint64_t received_at_open_{0};
    std::uint64_t retry_after_ns_{0};
    std::uint32_t reads_since_post_{0};
    State state_{State::Idle};
    bool running_{false};
    bool dead_{false};
    bool writing_{false};
    bool ping_pending_{false};
    bool awaiting_pong_{false};
    bool attempting_{false};
    bool refused_{false};
    bool healthy_reported_{false};
    std::chrono::steady_clock::time_point ping_sent_{};
};

} // namespace internal

struct WsClient::Impl {
    Impl(asio::io_context& ioc, WsConfig config, WsHandler& handler)
        : session{std::make_shared<internal::WsSession>(ioc, std::move(config), handler)} {}

    std::shared_ptr<internal::WsSession> session;
};

WsClient::WsClient(IoContext& io, WsConfig config, WsHandler& handler)
    : impl_{std::make_unique<Impl>(*static_cast<asio::io_context*>(io.native()), std::move(config), handler)} {}

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

std::string_view WsClient::close_detail() const noexcept {
    return impl_->session->close_detail();
}

} // namespace raikiri::net
