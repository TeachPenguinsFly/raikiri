#pragma once

// channels are "<SYMBOL>@<name>", case-insensitive; weex pings every ~30 s and expects k_pong_frame

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// weex offers 15 and 200; the book holds 200 and publishes the top k_book_levels
inline constexpr std::string_view k_depth_channel = "@depth200";
inline constexpr std::string_view k_trade_channel = "@trade";
inline constexpr std::string_view k_ticker_channel = "@ticker";

// weex does not echo the ping's timestamp
inline constexpr std::string_view k_pong_frame = R"({"method":"PONG","id":1})";

// empty for a kind weex does not stream; Bbo and Depth share one channel, so place one key
[[nodiscard]] std::string_view
channel_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> channels);

    // only followed by a subscribe of the same channel, to reseed its book
    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const std::string_view> channels);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    method_frame(std::string_view head, std::span<const std::string_view> channels);

    std::array<char, 2048> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::weex
