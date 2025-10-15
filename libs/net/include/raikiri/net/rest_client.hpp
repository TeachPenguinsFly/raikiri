#pragma once

// Warm-pool TLS REST client for order entry and cold reference pulls.
//
// Order submission rides HTTP for every venue we ship first, so the whole
// point of this class is that the expensive parts are already done when an
// order arrives: connections are established, TLS is negotiated, and the
// socket is kept alive. Firing an order is then a write on a hot socket.
// That is the "speed through preparation" thesis in its most literal form.
//
// Asynchronous by necessity: the gateway loop cannot block on a venue's
// round trip while cancels wait in a ring. Requests take a completion
// callback that fires from IoContext::poll() on the same thread.

#include <raikiri/common/time.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::net {

class IoContext;

enum class HttpMethod : std::uint8_t { Get, Post, Put, Delete, Patch };

struct RestConfig {
    std::string host;
    std::string port{"443"};
    std::size_t pool_size{2}; // warm connections held open
    std::uint32_t timeout_ms{5'000};
    std::uint32_t connect_timeout_ms{3'000};
    bool verify_tls{true};

    // The longest a send() that returned true can take to reach its callback.
    //
    // Two bounds in series, each of them timeout_ms: the wait for a free
    // connection, then the round trip on it. A caller that waits for the answer
    // under a deadline of its own MUST allow more than this. An equal deadline
    // is a coin flip between the two timers, and the half this one loses
    // abandons a request the pool is still holding.
    [[nodiscard]] constexpr std::uint64_t max_wait_ms() const noexcept {
        return std::uint64_t{timeout_ms} * 2;
    }
};

struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

struct RestRequest {
    HttpMethod method{HttpMethod::Post};
    std::string_view target;         // "/v5/order/create?a=b"
    std::string_view body;           // JSON, usually from JsonWriter
    std::vector<HttpHeader> headers; // signature, api key, timestamps
    std::string_view content_type{"application/json"};
};

// A transport-level failure (no HTTP status) reports status == 0 and a
// non-empty error; adapters map that to ErrorClass::Connectivity and
// synthesize a Rejected order event rather than leaving the order in doubt.
// What the venue said about OUR budget on this response.
//
// Every venue that meters us states the remaining allowance on every signed
// answer, and until now the transport threw it away and the budget was a
// number somebody typed into a yaml file from a documentation page. The
// venue's own word is the only figure that cannot go stale.
//
// Name and value verbatim: `X-MBX-USED-WEIGHT-1M`, `X-Bapi-Limit-Status`,
// `x-gate-ratelimit-requests-remain` all mean the same thing and none of them
// mean it the same way, so the transport carries the pair and whoever knows
// the venue is what reads it.
struct RateHeader {
    std::string name;
    std::string value;
};

struct RestResponse {
    int status{0};
    std::string body;
    std::string error;
    UnixNanos sent{};
    UnixNanos received{};
    std::vector<RateHeader> rate_headers;

    [[nodiscard]] bool transport_failed() const noexcept { return status == 0; }
};

using RestCallback = std::function<void(RestResponse&&)>;

// Every rate header this process saw, as the venue spelled it.
//
// PROCESS-WIDE ON PURPOSE: a gateway process is one venue, so every REST
// answer it receives is that venue talking about our budget with it. Threading
// a callback through sixteen adapters that have no opinion about metering
// would be ceremony for a fact the topology already settles.
//
// Diagnostic only — nothing throttles on this. It exists so a budget in
// `config/venues.yaml` can be measured from what the venue said rather than
// copied off a documentation page.
using RateObserver = std::function<void(std::string_view host, const RateHeader& header)>;

void set_rate_observer(RateObserver observer);

// EVERY REQUEST THIS PROCESS MAKES, as it completes.
//
// The rate observer above reports what a venue said about our budget; this
// reports what we actually spent it on. Without it, an IP ban is a message
// with no way to find the caller that earned it — the market-data pools are
// not metered by RateBudget at all, so their traffic is invisible to every
// other accounting in the process.
//
// `target` keeps its query string: two calls to one path with different
// parameters can cost very different weight.
using RequestObserver = std::function<void(
    std::string_view host, std::string_view target, int status, std::uint64_t elapsed_ns)>;

void set_request_observer(RequestObserver observer);

class RestClient {
public:
    RestClient(IoContext& io, RestConfig config);
    ~RestClient();
    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    // Opens the pool and keeps it warm. Safe to call before any request.
    void start();
    void stop();

    [[nodiscard]] bool send(const RestRequest& request, RestCallback on_complete);

    // ASKED BEFORE EVERY SEND, and a false refuses the request.
    //
    // The order path spends a RateBudget; the market-data pools spent nothing,
    // and their traffic was invisible to every counter in the process. That is
    // what banned this IP twice on 2026-09-06 — 342 book seeds at weight 20
    // against an allowance of 2400 a minute, from a path nothing was watching.
    //
    // A gate that refuses is not an error: the caller asks again on its next
    // tick, exactly as it does when the connection pool is busy. `target` is
    // the request line, so a gate can charge one endpoint more than another.
    using Admit = std::function<bool(std::string_view target)>;
    void set_admit(Admit admit);

    [[nodiscard]] std::size_t in_flight() const noexcept;

    // RestConfig::max_wait_ms, for the callers that hold a client and not the
    // config it was built from.
    [[nodiscard]] std::uint64_t max_wait_ms() const noexcept;

    void keepalive(UnixNanos now);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
