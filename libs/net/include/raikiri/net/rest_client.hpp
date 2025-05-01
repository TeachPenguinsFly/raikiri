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

enum class HttpMethod : std::uint8_t { Get, Post, Put, Delete };

struct RestConfig {
    std::string host;
    std::string port{"443"};
    std::size_t pool_size{2}; // warm connections held open
    std::uint32_t timeout_ms{5'000};
    std::uint32_t connect_timeout_ms{3'000};
    bool verify_tls{true};
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
struct RestResponse {
    int status{0};
    std::string body;
    std::string error;
    UnixNanos sent{};
    UnixNanos received{};

    [[nodiscard]] bool transport_failed() const noexcept { return status == 0; }
};

using RestCallback = std::function<void(RestResponse&&)>;

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

    [[nodiscard]] std::size_t in_flight() const noexcept;

    void keepalive(UnixNanos now);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
