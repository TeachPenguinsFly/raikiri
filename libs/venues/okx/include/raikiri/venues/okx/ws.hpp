#pragma once

// OKX v5 public websocket protocol: channel naming and subscription frames.
//
// A subscription is an OBJECT, not a stream name: {"channel":..,"instId":..}
// inside an args array. Nothing is baked into a path, so one socket carries
// every channel and the endpoint never changes.
//
// Keepalive is the bare text "ping" — not JSON, and OKX documents a 30 s idle
// timeout on application messages, so a quiet contract needs one. It must be
// exactly those four bytes: verified live, "ping" answers "pong" while
// "ping\n" is refused with 60012 Illegal request.

#include <raikiri/common/time.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>

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
    NotSigned,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// The demo endpoint is a different host serving a simulated book (verified:
// it answers, with its own prices). REST demo is a header on the same host
// instead, which is why only this side takes the flag.
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;

// Returns empty for a kind OKX does not stream under these names.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// A subscription argument is an object, and which key identifies the target
// depends on the channel: public channels name one instId, the private
// `orders` channel names an instType and covers every instrument under it.
// Sending instId to `orders` narrows the stream to one symbol, which is how a
// gateway ends up blind to fills on everything else it quotes.
struct Subscription {
    std::string_view channel{};
    // BRACED: a subscription that names only its channel is the ordinary
    // public case, and a designated initialiser that leaves these out must not
    // be a warning gcc makes fatal.
    std::string_view inst_id{};
    std::string_view inst_type{};
};

// The private order channel, per instrument type.
inline constexpr std::string_view k_orders_channel = "orders";

// The only public channel stating a 24-hour turnover. It has no SubKind of
// its own: OKX splits across three channels what bybit and gate put on one
// ticker frame, so this rides along with the funding subscription rather than
// letting the turnover column stay blank on every OKX row.
inline constexpr std::string_view k_tickers_channel = "tickers";

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

    // {"op":"login","args":[{"apiKey":..,"passphrase":..,"timestamp":..,"sign":..}]}
    //
    // Nothing about this signature resembles the REST one. The timestamp is
    // Unix Epoch SECONDS, not the ISO-8601 string the headers carry, and the
    // message is timestamp + "GET" + "/users/self/verify" — a path no request
    // ever goes to. OKX expires the frame 30 s after that timestamp.
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    login_frame(const net::Signer& signer, std::string_view passphrase, UnixNanos now);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

    // Exposed for tests: the exact bytes signed by the last login_frame.
    [[nodiscard]] std::string_view last_login_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const Subscription> subscriptions);

    std::array<char, 8192> frame_{};
    std::array<char, 64> login_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::size_t login_msg_len_{0};
};

} // namespace raikiri::venues::okx
