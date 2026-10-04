#pragma once

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// signed contract count; negative is short
struct WirePosition {
    std::string_view symbol;
    std::string_view current_qty;
    std::string_view avg_entry_price;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

// GET /api/v1/positions: data is the array itself
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

// a closed position, read only to learn which symbols may still owe funding
struct WireClosedPosition {
    std::string_view symbol;
    std::uint64_t close_ms{0};
};

inline constexpr std::size_t k_closed_position_page = 200;

struct ClosedPositionBuffer {
    std::array<WireClosedPosition, k_closed_position_page> positions{};
};

struct WireClosedPositionPage {
    std::span<const WireClosedPosition> positions;
    std::uint64_t total_pages{0};
};

// GET /api/v1/history-positions: data.items, paged by pageId
[[nodiscard]] std::expected<WireClosedPositionPage, WireError> parse_history_positions(std::string_view body,
                                                                                       ClosedPositionBuffer& storage);

} // namespace raikiri::venues::kucoin
