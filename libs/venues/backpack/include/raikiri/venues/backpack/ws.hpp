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
// 200 ms aggregate: the same chain with fewer frames
inline constexpr std::string_view k_depth_stream = "depth.200ms.";

[[nodiscard]] std::string_view stream_prefix(exec::SubKind kind) noexcept;

[[nodiscard]] std::string_view
stream_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept;

[[nodiscard]] std::string_view depth_symbol(std::string_view key) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, std::string_view>
    subscribe_frame(std::span<const std::string_view> streams);

private:
    std::array<char, 4096> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::backpack
