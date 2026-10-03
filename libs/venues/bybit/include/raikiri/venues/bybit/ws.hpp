#pragma once

// private auth signs "GET/realtime" plus an expiry that must be in the future

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

// perps and spot use separate sockets
[[nodiscard]] std::string_view public_stream_path(MarketType market) noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;

[[nodiscard]] std::string_view host(bool testnet) noexcept;

// bybit publishes fixed tiers and an unlisted depth fails the whole subscribe frame;
// bbo shares the topic, so a deeper tier slows the quote
inline constexpr std::uint32_t k_book_depth = 50;

// tiers bybit publishes on linear; other depths are refused
inline constexpr std::array<std::uint32_t, 4> k_book_tiers{1, 50, 200, 1000};

[[nodiscard]] constexpr bool is_book_tier(std::uint32_t depth) noexcept {
    return std::ranges::find(k_book_tiers, depth) != k_book_tiers.end();
}

// options publish only these, and print by base coin rather than by symbol
inline constexpr std::uint32_t k_option_book_depth = 25;

// `depth` must be one of k_book_tiers; empty for a kind bybit does not stream
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

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::span<const std::string_view> topics);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::span<const std::string_view> topics);

    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    auth_frame(const net::Signer& signer, UnixNanos now, std::uint32_t validity_ms = 5'000);

    [[nodiscard]] std::string_view ping_frame() const noexcept { return R"({"op":"ping"})"; }

    // exposed for tests: the exact bytes signed by the last auth_frame
    [[nodiscard]] std::string_view last_auth_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError> op_frame(std::string_view op,
                                                              std::span<const std::string_view> topics);

    std::array<char, 1024> frame_{};
    std::array<char, 64> auth_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t auth_msg_len_{0};
};

} // namespace raikiri::venues::bybit
