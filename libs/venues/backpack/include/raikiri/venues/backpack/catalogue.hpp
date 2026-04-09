#pragma once

// GET /api/v1/markets: every spot pair and perpetual on one list.
//
// Read live 2026-09-24: 190 rows, 103 PERP and 87 SPOT, all quoted in USDC.
// Only 143 of them are `orderBookState: Open` — 91 perpetuals and 52 pairs —
// and the rest are Closed (46) or PostOnly (1, and invisible). A Closed row
// still has a symbol, a tick and a step, which is how a catalogue ends up
// listing markets nothing can trade.

#include <raikiri/venues/backpack/envelope.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::backpack {

struct WireMarket {
    std::string_view symbol; // SOL_USDC, SOL_USDC_PERP, NVDA.US_USDC_PERP
    std::string_view base_asset;
    std::string_view quote_asset;
    std::string_view market_type;      // SPOT, PERP
    std::string_view order_book_state; // Open, Closed, PostOnly
    std::string_view tick_size;        // filters.price.tickSize
    std::string_view step_size;        // filters.quantity.stepSize
    // MILLISECONDS, stated per perpetual and null on a spot row. 0 means the
    // row did not say, and stays 0 all the way to the board.
    std::uint64_t funding_interval_ms{0};
};

// Room for the whole list twice over; the venue lists 190.
inline constexpr std::size_t k_max_markets = 512;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError>
parse_markets(std::string_view body, MarketBuffer& storage);

} // namespace raikiri::venues::backpack
