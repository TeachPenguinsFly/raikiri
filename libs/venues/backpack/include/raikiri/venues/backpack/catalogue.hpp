#pragma once

#include <raikiri/venues/backpack/envelope.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::backpack {

struct WireMarket {
    std::string_view symbol;
    std::string_view base_asset;
    std::string_view quote_asset;
    std::string_view market_type;
    std::string_view rwa_market_type; // null on crypto, STOCK, INDEX
    std::string_view order_book_state;
    std::string_view tick_size; // filters.price.tickSize
    std::string_view step_size; // filters.quantity.stepSize
    // Null on spot; 0 means unstated and stays 0 all the way to the board.
    std::uint64_t funding_interval_ms{0};
};

inline constexpr std::size_t k_max_markets = 512;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError>
parse_markets(std::string_view body, MarketBuffer& storage);

} // namespace raikiri::venues::backpack
