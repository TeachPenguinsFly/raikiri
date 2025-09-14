#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/trust_store.hpp>

#include <openssl/ssl.h>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::net {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

using TlsStream = beast::ssl_stream<beast::tcp_stream>;

constexpr std::string_view k_user_agent = "raikiri/0.1";

// Header names a venue states its metering under. Matched by PREFIX and
// case-insensitively, because the spellings differ by venue and by endpoint
// family within one venue: binance uses `X-MBX-USED-WEIGHT-1M` and
// `X-MBX-ORDER-COUNT-10S`, bybit `X-Bapi-Limit`, `X-Bapi-Limit-Status` and
// `X-Bapi-Limit-Reset-Timestamp`, gate `x-gate-ratelimit-*`, bitmex and bingx
// the conventional `x-ratelimit-*`, kucoin `gw-ratelimit-*`.
//
// A prefix list rather than an exact list: a venue that adds a fourth field to
// its family should be seen, not silently dropped.
constexpr std::string_view k_rate_header_prefixes[] = {
    "x-mbx-",
    "x-bapi-limit",
    "x-gate-ratelimit",
    "x-ratelimit",
    "gw-ratelimit",
    "ratelimit",
    "retry-after",
};

[[nodiscard]] bool is_rate_header(std::string_view name) noexcept {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char c : name) {
        lowered.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    for (const auto prefix : k_rate_header_prefixes) {
        if (lowered.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

// Set once at startup, read on the reactor thread that owns every REST
// completion. Not atomic because there is only ever the one writer and it runs
// before any client exists; a second writer would be a bug this cannot hide.
RateObserver g_rate_observer; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

template <typename Response>
void collect_rate_headers(const Response& res, std::vector<RateHeader>& out) {
    for (const auto& field : res) {
        const auto name = std::string_view{field.name_string().data(), field.name_string().size()};
        if (!is_rate_header(name)) {
            continue;
        }
        out.push_back({std::string{name}, std::string{field.value().data(), field.value().size()}});
    }
}

// Venues reap sockets that have been silent for a minute or so without ever
// saying that they did. Finding out on an order costs a full connect plus TLS
// at precisely the moment latency matters, so idle sockets get replaced on
// our schedule instead of theirs.
constexpr std::uint64_t k_idle_reconnect_ns = 30'000'000'000ULL;

// Waiters allowed per pooled connection before send() starts refusing. Deep
// enough to ride out one slow response, shallow enough that a stalled venue
// is reported as down instead of silently accumulating orders that will be
// stale by the time they leave.
constexpr std::size_t k_queue_depth_per_conn = 8;

constexpr std::size_t k_no_slot = static_cast<std::size_t>(-1);

[[nodiscard]] bool is_ip_literal(const std::string& host) noexcept {
    boost::system::error_code ec;
    static_cast<void>(asio::ip::make_address(host, ec));
    return !ec;
}

[[nodiscard]] http::verb to_verb(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::Get:
        return http::verb::get;
    case HttpMethod::Post:
        return http::verb::post;
    case HttpMethod::Put:
        return http::verb::put;
    case HttpMethod::Delete:
        return http::verb::delete_;
    case HttpMethod::Patch:
        return http::verb::patch;
    }
    return http::verb::post;
}

} // namespace

namespace detail {

// Lifetime works the same way as in WsClient: every completion handler holds a
// shared_ptr to the pool so the reactor can drain queued handlers after the
// RestClient is gone, a `dead_` flag makes those handlers no-ops so no
// callback reaches a destroyed caller, and each connection attempt owns a
// separate Conn kept alive by its own handlers. The last part matters more
// here than for websockets: an in-flight request's serialized bytes live in
// Conn::req, and a socket that failed cannot be reused anyway — an
// ssl::stream has no way back to a usable state once its handshake or
// transport breaks — so recovery is always a fresh Conn, never a reset one.
class RestPool : public std::enable_shared_from_this<RestPool> {
public:
    RestPool(asio::io_context& ioc, RestConfig config)
        : ioc_{ioc}, cfg_{std::move(config)}, tls_{ssl::context::tls_client}, resolver_{ioc},
          slots_(cfg_.pool_size == 0 ? std::size_t{1} : cfg_.pool_size) {
        if (cfg_.verify_tls) {
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
        host_header_ = cfg_.port == "443" ? cfg_.host : cfg_.host + ":" + cfg_.port;
    }

    void start() {
        if (dead_ || started_) {
            return;
        }
        started_ = true;
        ensure_connections(true);
    }

    void stop() {
        started_ = false;
        resolver_.cancel();
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            release_slot(i);
        }
        queue_.clear();
        dispatched_ = 0;
    }

    void shutdown() noexcept {
        dead_ = true;
        try {
            stop();
        } catch (...) {
            // Destructor path: nothing left to report to.
        }
    }

    [[nodiscard]] bool send(const RestRequest& request, RestCallback on_complete) {
        if (dead_) {
            return false;
        }
        Pending pending{build_request(request), std::move(on_complete), clock_.wall()};

        // Dispatching straight into async_write keeps the hot path one syscall deep.
        if (const std::size_t idx = find_free_slot(); idx != k_no_slot) {
            dispatch(idx, std::move(pending));
            return true;
        }
        if (queue_.size() >= slots_.size() * k_queue_depth_per_conn) {
            return false;
        }
        queue_.push_back(std::move(pending));
        ensure_connections(false);
        return true;
    }

    [[nodiscard]] std::size_t in_flight() const noexcept { return dispatched_ + queue_.size(); }

    void keepalive(UnixNanos now) {
        if (dead_) {
            return;
        }
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const auto& conn = slots_[i].conn;
            if (!conn || !conn->ready || conn->busy) {
                continue;
            }
            if (now.ns > conn->last_used.ns && now.ns - conn->last_used.ns > k_idle_reconnect_ns) {
                release_slot(i);
            }
        }
        ensure_connections(true);
    }

private:
    struct Conn {
        Conn(asio::io_context& ioc, ssl::context& tls) : stream{ioc.get_executor(), tls} {}

        TlsStream stream;
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::response<http::string_body> res;
        RestCallback cb;
        UnixNanos sent{};
        UnixNanos last_used{};
        bool ready{false};
        bool busy{false};
    };

    struct Slot {
        std::shared_ptr<Conn> conn;
        std::uint64_t generation{0};
    };

    struct Pending {
        http::request<http::string_body> req;
        RestCallback cb;
        UnixNanos queued{};
    };

    [[nodiscard]] bool
    current(std::size_t idx, std::uint64_t gen, const std::shared_ptr<Conn>& conn) const noexcept {
        return !dead_ && idx < slots_.size() && slots_[idx].generation == gen &&
               slots_[idx].conn == conn;
    }

    [[nodiscard]] std::size_t find_free_slot() const noexcept {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const auto& conn = slots_[i].conn;
            if (conn && conn->ready && !conn->busy) {
                return i;
            }
        }
        return k_no_slot;
    }

    [[nodiscard]] http::request<http::string_body> build_request(const RestRequest& request) const {
        http::request<http::string_body> req{to_verb(request.method), request.target, 11};
        req.set(http::field::host, host_header_);
        req.set(http::field::user_agent, k_user_agent);
        if (!request.content_type.empty()) {
            req.set(http::field::content_type, request.content_type);
        }
        for (const auto& header : request.headers) {
            req.set(header.name, header.value);
        }
        req.keep_alive(true);
        req.body() = std::string{request.body};
        req.prepare_payload();
        return req;
    }

    void ensure_connections(bool eager) {
        if (dead_) {
            return;
        }
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].conn && (eager || !queue_.empty())) {
                connect_slot(i);
            }
        }
    }

    void connect_slot(std::size_t idx) {
        if (dead_ || slots_[idx].conn) {
            return;
        }
        const auto gen = ++generation_;
        slots_[idx].generation = gen;
        auto conn = std::make_shared<Conn>(ioc_, tls_);
        slots_[idx].conn = conn;
        resolver_.async_resolve(
            cfg_.host,
            cfg_.port,
            [self = shared_from_this(), idx, gen, conn](
                const beast::error_code& ec, const tcp::resolver::results_type& results) {
                self->on_resolve(idx, gen, conn, ec, results);
            });
    }

    void on_resolve(std::size_t idx,
                    std::uint64_t gen,
                    const std::shared_ptr<Conn>& conn,
                    const beast::error_code& ec,
                    const tcp::resolver::results_type& results) {
        if (!current(idx, gen, conn)) {
            return;
        }
        if (ec) {
            on_setup_failed(idx, ec);
            return;
        }
        auto& lowest = beast::get_lowest_layer(conn->stream);
        lowest.expires_after(std::chrono::milliseconds{cfg_.connect_timeout_ms});
        lowest.async_connect(results,
                             [self = shared_from_this(), idx, gen, conn](
                                 const beast::error_code& connect_ec, const tcp::endpoint&) {
                                 self->on_connect(idx, gen, conn, connect_ec);
                             });
    }

    void on_connect(std::size_t idx,
                    std::uint64_t gen,
                    const std::shared_ptr<Conn>& conn,
                    const beast::error_code& ec) {
        if (!current(idx, gen, conn)) {
            return;
        }
        if (ec) {
            on_setup_failed(idx, ec);
            return;
        }
        if (!is_ip_literal(cfg_.host) &&
            SSL_set_tlsext_host_name(conn->stream.native_handle(), cfg_.host.c_str()) != 1) {
            on_setup_failed(idx, beast::error_code{asio::error::invalid_argument});
            return;
        }
        conn->stream.async_handshake(
            ssl::stream_base::client,
            [self = shared_from_this(), idx, gen, conn](const beast::error_code& tls_ec) {
                self->on_tls(idx, gen, conn, tls_ec);
            });
    }

    void on_tls(std::size_t idx,
                std::uint64_t gen,
                const std::shared_ptr<Conn>& conn,
                const beast::error_code& ec) {
        if (!current(idx, gen, conn)) {
            return;
        }
        if (ec) {
            on_setup_failed(idx, ec);
            return;
        }
        beast::get_lowest_layer(conn->stream).expires_never();
        conn->ready = true;
        conn->last_used = clock_.wall();
        pump();
    }

    void on_setup_failed(std::size_t idx, const beast::error_code& ec) {
        release_slot(idx);
        fail_queue(describe(ec));
    }

    void dispatch(std::size_t idx, Pending&& pending) {
        auto conn = slots_[idx].conn;
        const auto gen = slots_[idx].generation;

        conn->busy = true;
        conn->cb = std::move(pending.cb);
        conn->req = std::move(pending.req);
        conn->res = {};
        conn->buffer.clear();
        conn->sent = clock_.wall();
        ++dispatched_;

        beast::get_lowest_layer(conn->stream)
            .expires_after(std::chrono::milliseconds{cfg_.timeout_ms});
        http::async_write(
            conn->stream,
            conn->req,
            [self = shared_from_this(), idx, gen, conn](const beast::error_code& ec, std::size_t) {
                self->on_write(idx, gen, conn, ec);
            });
    }

    void on_write(std::size_t idx,
                  std::uint64_t gen,
                  const std::shared_ptr<Conn>& conn,
                  const beast::error_code& ec) {
        if (!current(idx, gen, conn)) {
            return;
        }
        if (ec) {
            fail_in_flight(idx, conn, ec);
            return;
        }
        http::async_read(conn->stream,
                         conn->buffer,
                         conn->res,
                         [self = shared_from_this(), idx, gen, conn](
                             const beast::error_code& read_ec, std::size_t) {
                             self->on_read(idx, gen, conn, read_ec);
                         });
    }

    void on_read(std::size_t idx,
                 std::uint64_t gen,
                 const std::shared_ptr<Conn>& conn,
                 const beast::error_code& ec) {
        if (!current(idx, gen, conn)) {
            return;
        }
        if (ec) {
            fail_in_flight(idx, conn, ec);
            return;
        }
        RestResponse response;
        response.status = static_cast<int>(conn->res.result_int());
        response.body = std::move(conn->res.body());
        response.sent = conn->sent;
        response.received = clock_.wall();
        collect_rate_headers(conn->res, response.rate_headers);
        if (g_rate_observer) {
            for (const auto& header : response.rate_headers) {
                g_rate_observer(cfg_.host, header);
            }
        }

        const bool reusable = conn->res.keep_alive();
        conn->busy = false;
        conn->last_used = response.received;
        auto callback = std::move(conn->cb);
        conn->cb = nullptr;
        --dispatched_;

        if (reusable) {
            beast::get_lowest_layer(conn->stream).expires_never();
        } else {
            release_slot(idx);
        }
        invoke(callback, std::move(response));
        pump();
    }

    void fail_in_flight(std::size_t idx,
                        const std::shared_ptr<Conn>& conn,
                        const beast::error_code& ec) {
        RestResponse response;
        response.status = 0;
        response.error = describe(ec);
        response.sent = conn->sent;
        response.received = clock_.wall();

        conn->busy = false;
        auto callback = std::move(conn->cb);
        conn->cb = nullptr;
        --dispatched_;
        release_slot(idx);
        invoke(callback, std::move(response));
        pump();
    }

    void fail_queue(const std::string& what) {
        auto pending = std::move(queue_);
        queue_.clear();
        for (auto& item : pending) {
            RestResponse response;
            response.status = 0;
            response.error = what;
            response.sent = item.queued;
            response.received = clock_.wall();
            invoke(item.cb, std::move(response));
        }
    }

    void pump() {
        while (!queue_.empty()) {
            const std::size_t idx = find_free_slot();
            if (idx == k_no_slot) {
                break;
            }
            auto pending = std::move(queue_.front());
            queue_.pop_front();
            dispatch(idx, std::move(pending));
        }
        ensure_connections(false);
    }

    void release_slot(std::size_t idx) noexcept {
        auto& slot = slots_[idx];
        if (!slot.conn) {
            return;
        }
        beast::get_lowest_layer(slot.conn->stream).close();
        slot.conn.reset();
        slot.generation = ++generation_;
    }

    [[nodiscard]] std::string describe(const beast::error_code& ec) const {
        return cfg_.host + ":" + cfg_.port + " " + ec.message();
    }

    static void invoke(const RestCallback& callback, RestResponse&& response) noexcept {
        if (!callback) {
            return;
        }
        try {
            callback(std::move(response));
        } catch (...) {
        }
    }

    asio::io_context& ioc_;
    RestConfig cfg_;
    ssl::context tls_;
    tcp::resolver resolver_;
    LiveClock clock_;

    std::vector<Slot> slots_;
    std::deque<Pending> queue_;
    std::string host_header_;
    std::size_t dispatched_{0};
    std::uint64_t generation_{0};
    bool started_{false};
    bool dead_{false};
};

} // namespace detail

struct RestClient::Impl {
    Impl(asio::io_context& ioc, RestConfig config)
        : pool{std::make_shared<detail::RestPool>(ioc, std::move(config))} {}

    std::shared_ptr<detail::RestPool> pool;
};

void set_rate_observer(RateObserver observer) {
    g_rate_observer = std::move(observer);
}

RestClient::RestClient(IoContext& io, RestConfig config)
    : impl_{std::make_unique<Impl>(*static_cast<asio::io_context*>(io.native()),
                                   std::move(config))} {}

RestClient::~RestClient() {
    impl_->pool->shutdown();
}

void RestClient::start() {
    impl_->pool->start();
}

void RestClient::stop() {
    impl_->pool->stop();
}

bool RestClient::send(const RestRequest& request, RestCallback on_complete) {
    return impl_->pool->send(request, std::move(on_complete));
}

std::size_t RestClient::in_flight() const noexcept {
    return impl_->pool->in_flight();
}

void RestClient::keepalive(UnixNanos now) {
    impl_->pool->keepalive(now);
}

} // namespace raikiri::net
