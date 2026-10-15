#pragma once

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// steps derive from szDecimals; isDelisted is absent on a live asset
struct WireAsset {
    std::string_view name;
    std::int64_t sz_decimals{0};
    std::int64_t max_leverage{0};
    // position in `universe`, which is the order's asset id; delisted rows keep their slot
    std::int64_t index{0};
    bool delisted{false};
};

inline constexpr std::size_t k_max_assets = 512;

struct AssetBuffer {
    std::array<WireAsset, k_max_assets> assets{};
    // spot token index of the margin asset; 0 or absent is usdc
    std::int64_t collateral_token{0};
};

// a literal `null` body, sent with http 200 on some malformed queries, fails here
[[nodiscard]] std::expected<std::span<const WireAsset>, WireError> parse_meta(std::string_view body,
                                                                              AssetBuffer& storage);

// the first perpDexs entry is null for the main dex and is skipped
inline constexpr std::size_t k_max_perp_dexs = 64;

// a dex's place in perpDexs, null rows counted, numbers its assets: 100000 + 10000 * place + meta index
struct PerpDexBuffer {
    std::array<std::string_view, k_max_perp_dexs> names{};
    std::array<std::int64_t, k_max_perp_dexs> places{};
};

[[nodiscard]] std::expected<std::span<const std::string_view>, WireError> parse_perp_dexs(std::string_view body,
                                                                                          PerpDexBuffer& storage);

// rows look like [["xyz:TSLA","stocks"],...]; the main dex has none
struct WireCategory {
    std::string_view name;
    std::string_view category;
};

inline constexpr std::size_t k_max_perp_categories = 2048;

// rows view `text`; the next parse reuses its buffer
struct PerpCategoryBuffer {
    std::array<WireCategory, k_max_perp_categories> rows{};
    std::array<char, std::size_t{64} * 1024> text{};
};

[[nodiscard]] std::expected<std::span<const WireCategory>, WireError>
parse_perp_categories(std::string_view body, PerpCategoryBuffer& storage);

[[nodiscard]] std::expected<std::string_view, WireError> parse_token_name(std::string_view body, std::int64_t index);

// `name` is the wire coin, "@<index>" for every pair except PURR/USDC
struct WireSpotAsset {
    std::string_view name;
    std::string_view base; // base token name
    // quote token; some pairs settle in usdh, usdt0 or usde
    std::string_view quote;
    std::int64_t index{0};       // pair index; the order asset id is 10000 + this
    std::int64_t sz_decimals{0}; // the base token's; both steps derive from it
};

inline constexpr std::size_t k_max_spot_assets = 1024;

struct SpotAssetBuffer {
    std::array<WireSpotAsset, k_max_spot_assets> assets{};
};

// joins universe[] to tokens[] by index and drops a pair whose token is missing
[[nodiscard]] std::expected<std::span<const WireSpotAsset>, WireError> parse_spot_meta(std::string_view body,
                                                                                       SpotAssetBuffer& storage);

} // namespace raikiri::venues::hyperliquid
