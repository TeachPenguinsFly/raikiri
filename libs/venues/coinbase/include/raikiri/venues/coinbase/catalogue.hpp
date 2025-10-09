#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// quote_increment and base_increment are the tick and step THEMSELVES —
// decimal strings like "0.01" — not the count of precision digits bingx and
// mexc publish. Nothing has to be exponentiated to use them, which is what
// makes this mapper shorter than those.
struct WireProduct {
    std::string_view id; // "BTC-USD": hyphenated, unlike every other venue
    std::string_view base_currency;
    std::string_view quote_currency;
    std::string_view quote_increment;
    std::string_view base_increment;
    std::string_view min_market_funds; // quote-currency floor for an order
    std::string_view status;           // online, delisted
    bool trading_disabled{false};
    bool cancel_only{false};
    bool post_only{false};
    bool limit_only{false};
    bool auction_mode{false};
};

// 832 rows live today and the endpoint has no filter, so the ceiling is sized
// for growth; past it a body is a venue incident or an attempt to make us
// allocate.
inline constexpr std::size_t k_max_products = 4096;

struct ProductBuffer {
    std::array<WireProduct, k_max_products> products{};
};

[[nodiscard]] std::expected<std::span<const WireProduct>, WireError>
parse_products(std::string_view body, ProductBuffer& storage);

// /products/{id} answers with the bare object, not a one-element array.
[[nodiscard]] std::expected<WireProduct, WireError> parse_product(std::string_view body);

} // namespace raikiri::venues::coinbase
