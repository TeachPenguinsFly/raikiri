#pragma once

#include <raikiri/venues/phemex/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::phemex {

struct WireCurrency {
    std::string_view currency;
    std::uint32_t value_scale{0};
};

// A row of `products`: spot pairs and the coin-margined perps, whose numbers
// are integers scaled by 10^priceScale / 10^valueScale.
struct WireProduct {
    std::string_view symbol;
    std::string_view type; // "Spot" or "Perpetual"
    std::string_view status;
    std::string_view base;   // spot
    std::string_view quote;  // spot and perp
    std::string_view settle; // perp: the coin it margins and pays in
    std::uint32_t price_scale{0};
    std::uint32_t ratio_scale{0};
    std::uint32_t funding_interval_s{0};
    // Perpetual: raw JSON numbers (0.1, 1.0), kept as text so no double is made.
    std::string_view tick_size;
    std::string_view contract_size;
    std::int64_t lot_size{0};
    // Spot: already in scaled units.
    std::int64_t quote_tick_ev{0};
    std::int64_t base_tick_ev{0};
    std::int64_t min_order_value_ev{0};
    std::int64_t taker_fee_er{0};
    std::int64_t maker_fee_er{0};
    bool has_fees{false};
};

// A row of `perpProductsV2`: the USDT- and USDC-margined perps, whose numbers
// are decimal strings.
struct WirePerp {
    std::string_view symbol;
    std::string_view status;
    std::string_view sub_type; // Normal, TradFi, PreMarket
    std::string_view base;     // "BTC", "1000 SHIB"
    std::string_view quote;
    std::string_view tick_size;
    std::string_view qty_step;
    std::string_view min_order_value;
    std::uint32_t funding_interval_s{0};
};

inline constexpr std::size_t k_max_currencies = 4096;
inline constexpr std::size_t k_max_products = 4096;
inline constexpr std::size_t k_max_perps = 4096;

struct CatalogueBuffer {
    std::array<WireCurrency, k_max_currencies> currencies{};
    std::array<WireProduct, k_max_products> products{};
    std::array<WirePerp, k_max_perps> perps{};
};

struct WireCatalogue {
    std::span<const WireCurrency> currencies;
    std::span<const WireProduct> products;
    std::span<const WirePerp> perps;
    std::size_t malformed{0}; // rows skipped because a field had the wrong type
};

[[nodiscard]] std::expected<WireCatalogue, WireError> parse_products(std::string_view body,
                                                                     CatalogueBuffer& storage);

} // namespace raikiri::venues::phemex
