#pragma once

// OKX v5 public websocket protocol: channel naming and subscription frames.
// Message building only — no sockets, no state.
//
// Three differences from binance shape this file.
//
// A subscription is an OBJECT, not a stream name: {"channel":..,"instId":..}
// inside an args array. Nothing is baked into a path, so one socket carries
// every channel and the endpoint never changes.
//
// The budget that matters is not stream count but op-message count: OKX allows
// 480 subscribe/unsubscribe/login messages per hour per connection. That is
// why subscriptions batch instead of going out one per instrument — a
// per-instrument frame would exhaust an hour's budget on 480 symbols.
//
// Keepalive is the bare text "ping" — not JSON, and OKX documents a 30 s idle
// timeout on application messages, so a quiet contract needs one. It must be
// exactly those four bytes: verified live, "ping" answers "pong" while
// "ping\n" is refused with 60012 Illegal request.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// The demo endpoint is a different host serving a simulated book (verified:
// it answers, with its own prices). REST demo is a header on the same host
// instead, which is why only this side takes the flag.
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Returns empty for a kind OKX does not stream under these names.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view inst_id;
};

// A connection's subscription ceiling is generous; the op-message budget is
// the real constraint, so frames carry as many args as fit.
inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;
inline constexpr std::size_t k_max_subscriptions_per_frame = 64;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // {"op":"subscribe","args":[{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"}]}
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const Subscription> subscriptions);

    // Sized so a full batch of the longest channel/instId pair still fits:
    // BufferTooSmall costs a whole batch of subscriptions, silently.
    std::array<char, 8192> frame_{};
};

} // namespace raikiri::venues::okx
