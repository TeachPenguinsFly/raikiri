#pragma once

// The order book, and the top of it.
//
// There is no book class here and no sequencing to hold. VERIFIED against
// consecutive live frames: every l2Book message is a full snapshot — no action
// field, no sequence id, no previous-sequence id anywhere in the payload — so
// there is no continuity chain to maintain and no resync path to get wrong,
// and the newest frame just replaces what the gateway holds.

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// {"px":..,"sz":..,"n":..}. `n` is an order count the domain does not model.
struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

struct WireBbo {
    std::string_view coin;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t ts_ms{0};
};

// `data.bbo` is [bid, ask] and either is null on a side with nothing resting;
// a null side yields empty views rather than an error, and the mapper drops it.
[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// l2Book pushes 20 a side; headroom without letting a frame make us allocate.
inline constexpr std::size_t k_max_levels = 256;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// `data.levels` is [bids, asks], bids descending and asks ascending.
struct WireBook {
    std::string_view coin;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

} // namespace raikiri::venues::hyperliquid
