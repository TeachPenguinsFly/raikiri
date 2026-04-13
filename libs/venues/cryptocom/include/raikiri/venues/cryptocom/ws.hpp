#pragma once

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::cryptocom {

inline constexpr std::uint32_t k_book_depth = 50;
// SNAPSHOT_AND_UPDATE refuses 500 with 40003.
inline constexpr std::uint32_t k_book_update_ms = 100;

inline constexpr std::string_view k_book_prefix = "book.";
inline constexpr std::string_view k_trade_prefix = "trade.";
inline constexpr std::string_view k_ticker_prefix = "ticker.";
inline constexpr std::string_view k_funding_prefix = "funding.";
inline constexpr std::string_view k_estimated_prefix = "estimatedfunding.";
inline constexpr std::string_view k_mark_prefix = "mark.";

inline constexpr std::size_t k_max_channel_len = 64;

// Empty when the name does not fit `out`.
[[nodiscard]] std::string_view
channel_name(std::string_view prefix, std::string_view symbol, std::span<char> out) noexcept;

[[nodiscard]] std::string_view book_channel(std::string_view symbol, std::span<char> out) noexcept;

[[nodiscard]] std::string_view instrument_of(std::string_view channel) noexcept;

enum class WsBuildError : std::uint8_t {
    TooLarge,
};

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // As many channels as fit under ep::k_max_frame_bytes. The book mode rides
    // on every frame; without it a book channel gets the venue's default mode.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> channels);

    [[nodiscard]] std::string_view heartbeat_reply(std::int64_t id);

private:
    std::array<char, 4096> frame_{};
    std::array<char, 96> reply_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::cryptocom
