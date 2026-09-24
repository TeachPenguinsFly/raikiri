#pragma once

// only the depth snapshot is subscribed; depth_update is parsed but has no caller

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level on depth_update
};

inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// futures stamps the snapshot with digit-string id, spot with numeric i
struct WireDepth {
    std::string_view symbol;
    std::uint64_t update_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market);

// a delta applies when pu equals the previous u; fu is always pu + 1
struct WireDepthUpdate {
    std::string_view symbol;
    std::uint64_t first_update_id{0};
    std::uint64_t prev_update_id{0};
    std::uint64_t update_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view frame,
                                                                           LevelBuffer& storage);

} // namespace raikiri::venues::xt
