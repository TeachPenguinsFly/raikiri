#pragma once

// A small HTTP server, owned by no service in particular.
//
// It began as admind's half and moved out when a second process needed one:
// nothing here knows about a registry, a database or a contract, so a consumer
// that wants to serve JSON no longer inherits libpqxx to get a socket.
//
// Beast is already a dependency — every venue socket is built on it — so
// serving HTTP costs no new one. It stays behind this facade for the same
// reason IoContext does: no Boost type appears in a public header, so a
// consumer of this library never inherits the dependency.
//
// SINGLE-THREADED, DELIBERATELY. Handlers read shared-memory tables that a
// gateway is writing concurrently; a seqlock read is safe from any thread, but
// a thread pool here would buy nothing — an admin API serves a handful of
// browsers, and every answer is already one acquire-load away. One thread also
// means a handler can hold a Registry reference without a lock.

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
    std::string_view method;
    std::string_view path;
    std::string_view query; // raw, without the '?'
    std::string_view body;  // empty on GET; the parser caps it at 16 KiB

    // Named rather than held in a header map, because exactly one endpoint
    // reads a header and a map would allocate on every request to carry a
    // field almost none of them use. When a second header needs reading, that
    // is the moment to grow a general accessor — not before.
    std::string_view idempotency_key;
};

struct Response {
    int status{200};
    std::string content_type{"application/json"};
    std::string body;
};

// Returns the response for one request. Anything it throws becomes a 500 with
// an RFC 9457 body: a handler that dies must not take the server with it.
using Handler = std::function<Response(const Request&)>;

class Server {
public:
    Server(std::string address, std::uint16_t port, Handler handler);
    // Runs on `io` instead of a reactor of its own, so one wait serves both.
    Server(net::IoContext& io, std::string address, std::uint16_t port, Handler handler);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts accepting. Returns false when the port is taken, which
    // is a configuration error worth saying out loud rather than retrying.
    [[nodiscard]] bool start();

    // One pass over ready work. The caller owns the loop, exactly as gatewayd
    // owns its own — nothing here blocks on the network.
    std::size_t poll() noexcept;

    // Blocks on the socket until something arrives or `timeout` passes, then
    // runs whatever is ready. A loop of these answers a request as soon as it
    // lands; a loop of poll() and a sleep answers it a sleep later.
    std::size_t run_for(std::chrono::nanoseconds timeout) noexcept;

    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ONE BLOCKING GET, to a service on this host.
//
// Blocking on purpose and bounded on purpose: the caller is a single-threaded
// loop, and the only thing it may wait on is a peer close enough that waiting
// is measured in microseconds. `timeout_ms` is the whole exchange — connect,
// write, read — because a dependency that hangs must degrade the caller by a
// known amount rather than by however long a socket feels like taking.
//
// Nullopt is "did not answer", never an empty body: a caller has to be able to
// tell a service that is gone from one that returned nothing, and that
// distinction is the whole reason this returns an optional.
struct Fetched {
    int status{0};
    std::string body;
};

[[nodiscard]] std::optional<Fetched> fetch(std::string_view host,
                                           std::uint16_t port,
                                           std::string_view target,
                                           std::uint32_t timeout_ms) noexcept;

// RFC 9457, rendered. THE ONE ERROR SHAPE every API in this repository
// produces, and it lives here rather than beside one of them because a caller
// behind admind's proxy cannot tell which process refused — two renderings of
// the same shape would eventually differ in a field, and the difference would
// show up as a client that handles one service's errors and not another's.
//
// `detail` is escaped on the way out, which is the reason this is not three
// string concatenations: it carries text a caller sent us.
[[nodiscard]] Response problem(int status, std::string_view title, std::string_view detail = {});

// A query string's value for `key`, or empty. Percent-decoding is deliberately
// absent: every parameter this API takes is digits or an account name, and a
// decoder that is never exercised is a decoder that is wrong when it finally is.
[[nodiscard]] std::string_view query_param(std::string_view query, std::string_view key) noexcept;

} // namespace raikiri::http
