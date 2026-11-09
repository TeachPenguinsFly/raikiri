#pragma once

// single-threaded http server; boost stays behind this facade

#include <raikiri/net/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::http {

struct Request {
    std::string_view method{};
    std::string_view path{};
    std::string_view query{}; // raw, without the '?'
    std::string_view body{};  // empty on GET; the parser caps it at 16 KiB

    // named rather than a header map; one endpoint reads each
    std::string_view idempotency_key{};
    // who the proxy in front authenticated; it overwrites what a client sends, and empty is nobody it knows
    std::string_view login{};
};

struct Response {
    int status{200};
    std::string content_type{"application/json"};
    std::string body;
};

// anything the handler throws becomes a 500 with an rfc 9457 body
using Handler = std::function<Response(const Request&)>;

// the answer to one request, given later: from any thread, once; later calls do nothing, and a reply dropped without
// an answer sends a 500. The connection reads nothing more until it is answered
class Reply {
public:
    struct Pending;
    explicit Reply(std::shared_ptr<Pending> pending) noexcept;
    void operator()(Response response) const;

private:
    std::shared_ptr<Pending> pending_;
};

// for an answer that waits on something else, such as a database; the request's views die when the handler returns,
// so it copies what it keeps
using DeferredHandler = std::function<void(const Request&, Reply)>;

class Server {
public:
    Server(std::string address, std::uint16_t port, Handler handler);
    // runs on `io` so one wait serves both
    Server(net::IoContext& io, std::string address, std::uint16_t port, Handler handler);
    Server(net::IoContext& io, std::string address, std::uint16_t port, DeferredHandler handler);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // false when the port is taken
    [[nodiscard]] bool start();

    // one pass over ready work; nothing here blocks
    std::size_t poll() noexcept;

    // blocks until something arrives or `timeout` passes, then runs what is ready
    std::size_t run_for(std::chrono::nanoseconds timeout) noexcept;

    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// one blocking request to a local service; `timeout_ms` bounds the whole exchange
// nullopt means no answer, distinct from an empty body
struct Fetched {
    int status{0};
    std::string body;
};

[[nodiscard]] std::optional<Fetched> fetch(std::string_view host,
                                           std::uint16_t port,
                                           std::string_view target,
                                           std::uint32_t timeout_ms,
                                           std::string_view method = "GET",
                                           std::string_view body = {},
                                           std::string_view login = {}) noexcept;

// rfc 9457 problem response shared by every api here; `detail` is escaped
[[nodiscard]] Response problem(int status, std::string_view title, std::string_view detail = {});

// a query value for `key`, or empty; no percent-decoding
[[nodiscard]] std::string_view query_param(std::string_view query, std::string_view key) noexcept;

// query_param hands back raw text; a browser sends a name's ':' as %3A
[[nodiscard]] std::string percent_decoded(std::string_view text);

} // namespace raikiri::http
