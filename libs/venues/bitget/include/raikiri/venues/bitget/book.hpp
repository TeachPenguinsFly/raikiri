#pragma once

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// books snapshots 500 levels a side
inline constexpr std::size_t k_max_levels = 1024;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// an update applies when its prev_seq_id equals the previous seq_id; books15 pins prev_seq_id to 0
struct WireBooks {
    std::string_view inst_id;
    bool snapshot{false}; // action: "snapshot" vs "update"
    std::int64_t seq_id{0};
    std::int64_t prev_seq_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBooks, WireError> parse_books(std::string_view frame,
                                                              LevelBuffer& storage);

// its data is an object, unlike every other endpoint here
[[nodiscard]] std::expected<WireBooks, WireError> parse_rest_orderbook(std::string_view body,
                                                                       LevelBuffer& storage);

} // namespace raikiri::venues::bitget
