#pragma once

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::apex {

enum class WsBuildError : std::uint8_t {
    Empty,
    TooManyTopics,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// One refused topic refuses every topic in its frame, so a frame carries one
// instrument's topics and never more.
inline constexpr std::size_t k_max_topics_per_frame = 4;

inline constexpr std::string_view k_book_prefix = "orderBook25.H.";
inline constexpr std::string_view k_trade_prefix = "recentlyTrade.H.";
inline constexpr std::string_view k_instrument_prefix = "instrumentInfo.H.";

// Empty for a kind this adapter does not stream.
[[nodiscard]] std::string_view
topic_name(exec::SubKind kind, std::string_view cross_symbol, std::span<char> out) noexcept;

class Protocol {
public:
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    subscribe_frame(std::span<const std::string_view> topics);

    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    unsubscribe_frame(std::span<const std::string_view> topics);

    [[nodiscard]] std::expected<std::string_view, WsBuildError> pong_frame(std::string_view value);

private:
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    op_frame(std::string_view op, std::span<const std::string_view> args);

    std::array<char, 512> frame_{};
};

} // namespace raikiri::venues::apex
