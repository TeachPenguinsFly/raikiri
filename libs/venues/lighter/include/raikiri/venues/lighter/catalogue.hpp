#pragma once

// What the venue lists, off GET /api/v1/orderBookDetails.
//
// A MARKET IS AN INTEGER. Every channel on the socket is keyed by market_id
// and no frame but the ticker names a symbol, so the id is the key this
// adapter resolves everything by; the symbol is kept for people.
//
// Prices and sizes are decimals whose precision the row states as a count of
// places — price_decimals 1 on BTC is a tick of 0.1 — rather than as a step.

#include <raikiri/venues/lighter/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::lighter {

struct WireMarket {
    std::string_view symbol;      // "BTC" on a perp, "ETH/USDC" on spot
    std::string_view market_type; // "perp" or "spot"
    std::string_view status;      // "active" or "inactive"
    std::uint32_t market_id{0};
    std::uint32_t size_decimals{0};
    std::uint32_t price_decimals{0};
    std::string_view min_base_amount;
    std::string_view min_quote_amount;
    // PERCENT PER EIGHT HOURS, as the venue writes it ("4.0000" is 4%). Perps
    // only; empty on spot. See map_instrument for how it becomes an hourly cap.
    std::string_view funding_clamp_big;
};

inline constexpr std::size_t k_max_markets = 1024;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

// Both arrays, perps first. A row missing its id or its decimals fails the
// whole parse: a catalogue that loaded half its markets reads downstream as a
// venue that delisted the other half.
[[nodiscard]] std::expected<std::span<const WireMarket>, WireError>
parse_order_book_details(std::string_view body, MarketBuffer& storage);

} // namespace raikiri::venues::lighter
