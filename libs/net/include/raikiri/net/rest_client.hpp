#pragma once

// warm-pool tls rest client; completions fire from IoContext::poll() on the same thread

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

    // the longest a send() that returned true can take to reach its callback; a caller's deadline must exceed it
    [[nodiscard]] constexpr std::uint64_t max_wait_ms() const noexcept { return std::uint64_t{timeout_ms} * 2; }
};

struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

struct RestRequest {
    HttpMethod method{HttpMethod::Post};
    std::string_view target{}; // "/v5/order/create?a=b"
    std::string_view body{};   // JSON, usually from JsonWriter
    // braced so leaving it out of a designated initialiser is not a gcc warning
    std::vector<HttpHeader> headers{}; // signature, api key, timestamps
    std::string_view content_type{"application/json"};
};

// the venue's own rate header on this response, name and value verbatim
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

// every rate header this process saw; diagnostic only, process-wide
using RateObserver = std::function<void(std::string_view host, const RateHeader& header)>;

void set_rate_observer(RateObserver observer);

// every request this process makes, as it completes; `target` keeps its query string
using RequestObserver = std::function<void(
    std::string_view host, std::string_view target, int status, std::uint64_t elapsed_ns)>;

void set_request_observer(RequestObserver observer);

class RestClient {
public:
    RestClient(IoContext& io, RestConfig config);
    ~RestClient();
    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    // opens the pool and keeps it warm
    void start();
    void stop();

    [[nodiscard]] bool send(const RestRequest& request, RestCallback on_complete);

    // asked before every send; false refuses the request and the caller retries next tick
    using Admit = std::function<bool(std::string_view target)>;
    void set_admit(Admit admit);

    [[nodiscard]] std::size_t in_flight() const noexcept;

    // RestConfig::max_wait_ms, for callers without the config
    [[nodiscard]] std::uint64_t max_wait_ms() const noexcept;

    void keepalive(UnixNanos now);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
