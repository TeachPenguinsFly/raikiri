#pragma once

// TLS WebSocket client for venue market-data and user streams.
//
// What this owns, so that eight adapters do not each reimplement it (the
// duplication that made the predecessor system's twenty-seven adapters drift
// apart): TLS handshake, the reconnect state machine with exponential
// backoff and jitter, keepalive ping/pong, an outbound queue that never
// blocks the caller, and the on_open hook that replays subscriptions.
//
// What it deliberately does NOT own: subscription state. The gateway core
// keeps the desired set and re-asserts it from on_open(), because only the
// core knows what refcounting says should currently be live.
//
// on_message is the hot path: it receives a view into the read buffer that
// is invalidated on return, so a handler parses in place (simdjson
// on-demand) and never stores the pointer.

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
    std::size_t send_queue_capacity{256};
    std::size_t max_message_bytes{1 << 20};
    bool verify_tls{true}; // false only for local test servers
    // Asked before every connect and reconnect: zero connects now, otherwise
    // the nanoseconds to wait before asking again.
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

class WsHandler {
public:
    WsHandler() = default;
    WsHandler(const WsHandler&) = delete;
    WsHandler& operator=(const WsHandler&) = delete;

    virtual void on_open() = 0;

    // Hot path. `payload` is valid only for the duration of the call.
    virtual void on_message(std::string_view payload, UnixNanos received) noexcept = 0;

    // Followed by an automatic reconnect unless the reason is Requested.
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

    // Begins connecting. Progress happens inside IoContext::poll().
    void start();

    // Closes and suppresses reconnection. Idempotent.
    void stop();

    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] bool send(std::string_view text);

    // The cause behind the last on_close: "read: asio.misc 2 End of file",
    // "close 1001 going away". Empty for Requested. Valid until the next close.
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
