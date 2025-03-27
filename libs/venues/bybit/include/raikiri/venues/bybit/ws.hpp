#pragma once

// Bybit v5 websocket protocol: topic naming, subscription frames, and the
// private-stream handshake.
//
// Message building only — no sockets, no state. The transport lives in
// raikiri::net and the desired subscription set lives in the gateway core,
// which re-asserts it from on_open(). That split is what makes a reconnect
// and a cold start run identical code, and it is why this file has no
// notion of what is currently subscribed.
//
// The private handshake is its own signature scheme, unrelated to REST: the
// message is the literal "GET/realtime" followed by an expiry timestamp that
// must be in the FUTURE. Signing the current time produces a frame bybit
// rejects the instant it arrives, which reads as a credentials problem.

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,   // a subscription kind bybit does not offer
    UnknownInstrument,
    TooManyTopics,     // more args than one frame may carry
    BufferTooSmall,
    NotSigned,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// Public stream URLs by market. Perps and spot are separate sockets, which
// is also why one gateway process holds several connections.
[[nodiscard]] std::string_view public_stream_path(MarketType market) noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;

[[nodiscard]] std::string_view host(bool testnet) noexcept;

// Book depth to request. Bybit publishes fixed depths only; asking for an
// unsupported one yields silence rather than an error, so the choice is
// constrained here rather than left to configuration.
inline constexpr std::uint32_t k_book_depth = 50;

// Bybit caps the args array; oversized subscribe frames are rejected whole,
// so callers batch through subscribe_frame() until it returns the number
// consumed.
inline constexpr std::size_t k_max_topics_per_frame = 10;

// Builds "orderbook.50.BTCUSDT" and friends. Returns empty for a kind bybit
// does not stream.
[[nodiscard]] std::string_view topic_name(exec::SubKind kind, std::string_view exchange_symbol,
                                          std::span<char> out) noexcept;

class Protocol {
public:
    // Builds {"op":"subscribe","args":[...]} from as many of `topics` as fit
    // in one frame. Returns the frame and how many topics it consumed, so
    // the caller loops rather than guessing the batch size.
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> topics);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const std::string_view> topics);

    // The private-stream handshake. `now` is venue-aligned time; the expiry
    // sent is now + `validity_ms`, because bybit requires it to be ahead of
    // its own clock when the frame lands.
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    auth_frame(const net::Signer& signer, UnixNanos now, std::uint32_t validity_ms = 5'000);

    // Bybit runs its own application-level ping on top of the websocket
    // control frame, and drops connections that only answer the latter.
    [[nodiscard]] std::string_view ping_frame() const noexcept { return R"({"op":"ping"})"; }

    // Exposed for tests: the exact bytes signed by the last auth_frame.
    [[nodiscard]] std::string_view last_auth_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError> op_frame(std::string_view op,
                                                              std::span<const std::string_view>
                                                                  topics);

    std::array<char, 1024> frame_{};
    std::array<char, 64> auth_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t auth_msg_len_{0};
};

} // namespace raikiri::venues::bybit
