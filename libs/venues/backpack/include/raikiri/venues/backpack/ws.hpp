#pragma once

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::backpack {

inline constexpr std::string_view k_bbo_stream = "bookTicker.";
inline constexpr std::string_view k_trade_stream = "trade.";
inline constexpr std::string_view k_mark_price_stream = "markPrice.";
// The 200 ms aggregate, not realtime: the same chain at a fraction of the frames.
inline constexpr std::string_view k_depth_stream = "depth.200ms.";

inline constexpr std::size_t k_max_streams_per_connection = 1000;
inline constexpr std::size_t k_max_streams_per_frame = 50;

[[nodiscard]] std::string_view stream_prefix(exec::SubKind kind) noexcept;

// Empty for a kind this venue does not stream by symbol.
[[nodiscard]] std::string_view
stream_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept;

// The symbol a depth key carries, or empty when `key` is not a depth stream.
[[nodiscard]] std::string_view depth_symbol(std::string_view key) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // At most k_max_streams_per_frame of `streams`; call again with the rest.
    [[nodiscard]] std::expected<Frame, std::string_view>
    subscribe_frame(std::span<const std::string_view> streams);

private:
    std::array<char, 4096> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::backpack
