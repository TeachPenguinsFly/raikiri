#pragma once

// tls websocket client with reconnect, keepalive and a queued outbox; subscriptions belong to the caller
// on_message's payload is invalidated on return

#include <raikiri/common/time.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace raikiri::net {

class IoContext;

struct WsConfig {
    std::string host; // "stream.bybit.com"
    std::string port{"443"};
    std::string target{"/"}; // "/v5/public/linear"
    std::uint32_t ping_interval_ms{20'000};
    std::uint32_t pong_timeout_ms{10'000};
    std::uint32_t reconnect_min_ms{250};
    std::uint32_t reconnect_max_ms{30'000};
    // the least wait after a declined upgrade; a draining node keeps declining for seconds
    std::uint32_t declined_backoff_ms{5'000};
    std::size_t send_queue_capacity{256};
    std::size_t max_message_bytes{1 << 20};
    bool verify_tls{true}; // false only for local test servers
    // asked before every connect: zero connects now, otherwise nanoseconds to wait
    std::function<std::uint64_t()> connect_gate{};
};

enum class WsCloseReason : std::uint8_t {
    Requested,     // stop() was called; no reconnect
    Transport,     // connect/TLS/read failure
    PongTimeout,   // peer stopped answering keepalive
    ProtocolError, // oversize frame, malformed close
    ServerClosed,
};

[[nodiscard]] std::string_view to_string(WsCloseReason reason) noexcept;

// every keepalive pong with its ping's round trip; process-wide
using PongObserver = std::function<void(std::string_view host, std::uint64_t rtt_ns)>;
void set_pong_observer(PongObserver observer);

// linux only: how long each message's last segment sat after the kernel's receive stamp
using ArrivalObserver = std::function<void(std::string_view host, std::uint64_t queued_ns)>;
void set_arrival_observer(ArrivalObserver observer);

class WsHandler {
public:
    WsHandler() = default;
    WsHandler(const WsHandler&) = delete;
    WsHandler& operator=(const WsHandler&) = delete;

    virtual void on_open() = 0;

    // hot path; `payload` is valid only during the call
    virtual void on_message(std::string_view payload, UnixNanos received) noexcept = 0;

    // followed by a reconnect unless the reason is Requested
    virtual void on_close(WsCloseReason reason) = 0;

protected:
    ~WsHandler() = default;
};

class WsClient {
public:
    WsClient(IoContext& io, WsConfig config, WsHandler& handler);
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // begins connecting; progress happens inside IoContext::poll()
    void start();

    // closes and suppresses reconnection; idempotent
    void stop();

    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] bool send(std::string_view text);

    // the cause behind the last on_close; empty for Requested, valid until the next close
    [[nodiscard]] std::string_view close_detail() const noexcept;

    struct Stats {
        std::uint64_t messages_received{0};
        std::uint64_t messages_sent{0};
        std::uint64_t reconnects{0};
        std::uint64_t send_queue_full{0};
        std::uint64_t pong_timeouts{0};
    };

    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
