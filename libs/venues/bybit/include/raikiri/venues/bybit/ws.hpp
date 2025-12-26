#pragma once

// Bybit v5 websocket protocol: topic naming, subscription frames, and the
// private-stream handshake.
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

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind, // a subscription kind bybit does not offer
    UnknownInstrument,
    TooManyTopics, // more args than one frame may carry
    BufferTooSmall,
    NotSigned,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// Public stream URLs by market. Perps and spot are separate sockets, which
// is also why one gateway process holds several connections.
[[nodiscard]] std::string_view public_stream_path(MarketType market) noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;

[[nodiscard]] std::string_view host(bool testnet) noexcept;

// Book depth to request, and it is a CADENCE as much as a depth. Bybit
// publishes fixed tiers only — on linear, 1 at 10 ms, 50 at 20 ms, 200 at
// 100 ms and 1000 at 200 ms — and anything else is answered with
// `error:handler not found`, which kills the whole subscribe frame. So a
// requested tier is checked against this list before it goes out.
//
// The deeper tier is also the quieter one: measured across the whole linear
// catalogue on 2026-09-18, 811 symbols over two minutes, orderbook.50 sent
// 4394 messages a second and orderbook.200 sent 2244 — half the messages,
// 65% of the bytes, and still 90% of the level updates, because five 20 ms
// deltas on one symbol overlap and collapse into one at 100 ms.
//
// 50 STAYS THE DEFAULT because bybit serves bbo and depth from ONE topic, so
// the deeper tier slows the quote as well. That trade belongs to a deployment
// — config/venues.yaml — and not to everyone who links this.
inline constexpr std::uint32_t k_book_depth = 50;

// Tiers bybit actually publishes on linear. A depth request that is not one of
// these is refused rather than rounded: rounding would silently subscribe a
// cadence nobody asked for.
inline constexpr std::array<std::uint32_t, 4> k_book_tiers{1, 50, 200, 1000};

[[nodiscard]] constexpr bool is_book_tier(std::uint32_t depth) noexcept {
    return std::ranges::find(k_book_tiers, depth) != k_book_tiers.end();
}

// Bybit caps the args array; oversized subscribe frames are rejected whole,
// so callers batch through subscribe_frame() until it returns the number
// consumed.
inline constexpr std::size_t k_max_topics_per_frame = 10;

// What ONE connection may carry. Bybit states this ceiling in CHARACTERS
// rather than topics — one public connection may not hold an args array over
// 21,000 characters — and documents no topic count for linear at all, which is
// why it accepted the whole catalogue and then quoted 25 of 150 sampled
// instruments: those 2565 topics are 57,477 characters of args, and only the
// first 851 of them are inside the cap.
//
// The longest topic built here is 32 characters ("orderbook.50." and bybit's
// longest linear symbol), costing 35 in the array with its quotes and comma,
// so 512 of them is 17,920 — under the cap whatever mix lands on the socket.
inline constexpr std::size_t k_max_subscriptions_per_connection = 512;

// Builds "orderbook.50.BTCUSDT" and friends. Returns empty for a kind bybit
// does not stream. `depth` must be one of k_book_tiers.
[[nodiscard]] std::string_view topic_name(exec::SubKind kind,
                                          std::string_view exchange_symbol,
                                          std::span<char> out,
                                          std::uint32_t depth = k_book_depth) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> topics);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const std::string_view> topics);

    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    auth_frame(const net::Signer& signer, UnixNanos now, std::uint32_t validity_ms = 5'000);

    [[nodiscard]] std::string_view ping_frame() const noexcept { return R"({"op":"ping"})"; }

    // Exposed for tests: the exact bytes signed by the last auth_frame.
    [[nodiscard]] std::string_view last_auth_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const std::string_view> topics);

    std::array<char, 1024> frame_{};
    std::array<char, 64> auth_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t auth_msg_len_{0};
};

} // namespace raikiri::venues::bybit
