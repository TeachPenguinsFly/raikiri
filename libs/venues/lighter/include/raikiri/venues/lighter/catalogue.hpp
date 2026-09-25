#pragma once

#include <raikiri/venues/lighter/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::lighter {

struct WireMarket {
    std::string_view symbol; // "BTC" on a perp, "ETH/USDC" on spot
    std::string_view market_type;
    std::string_view status;
    std::uint32_t market_id{0};
    std::uint32_t size_decimals{0};
    std::uint32_t price_decimals{0};
    std::string_view min_base_amount;
    std::string_view min_quote_amount;
    // Percent per eight hours; empty on spot.
    std::string_view funding_clamp_big;
};

inline constexpr std::size_t k_max_markets = 1024;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

// A row missing its id or decimals fails the whole parse: half a catalogue would
// read downstream as delistings.
[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_order_book_details(std::string_view body,
                                                                                             MarketBuffer& storage);

} // namespace raikiri::venues::lighter
