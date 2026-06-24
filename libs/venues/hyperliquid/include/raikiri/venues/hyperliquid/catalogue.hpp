#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill. Perps and spot are two separate /info
// request types with two different shapes.

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// A row of {"type":"meta"}. The venue publishes DECIMALS, not steps, and both
// steps are derived from szDecimals in the mapper. isDelisted is ABSENT on a
// live asset rather than false; a delisted row keeps its slot — 55 of 232.
struct WireAsset {
    std::string_view name;
    std::int64_t sz_decimals{0};
    std::int64_t max_leverage{0};
    // Its POSITION in `universe`, which is the asset id an order names. A
    // delisted row keeps its slot forever, so this is NOT the row's rank among
    // the tradable assets, and deriving it by counting loaded rows shifts every
    // asset after MATIC onto its neighbour.
    std::int64_t index{0};
    bool delisted{false};
};

// 232 perps today, in one response; a builder-deployed DEX adds rows unasked.
inline constexpr std::size_t k_max_assets = 512;

struct AssetBuffer {
    std::array<WireAsset, k_max_assets> assets{};
    // The spot TOKEN index the dex margins in: 0 is USDC, and a HIP-3 dex may
    // name another. Absent on the main dex's meta, which reads as USDC.
    std::int64_t collateral_token{0};
};

// Reads the `universe` of a {"type":"meta"} body. A body of the bare literal
// `null` — which is how /info answers some malformed queries WITH HTTP 200 —
// fails here rather than yielding an empty catalogue.
[[nodiscard]] std::expected<std::span<const WireAsset>, WireError> parse_meta(std::string_view body,
                                                                              AssetBuffer& storage);

// The HIP-3 dex names of a {"type":"perpDexs"} body. The first entry is a
// literal null standing for the main dex and is skipped.
inline constexpr std::size_t k_max_perp_dexs = 64;

struct PerpDexBuffer {
    std::array<std::string_view, k_max_perp_dexs> names{};
};

[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_perp_dexs(std::string_view body, PerpDexBuffer& storage);

// The name of spot token `index` in a {"type":"spotMeta"} body, or
// MissingField when the body does not carry it.
[[nodiscard]] std::expected<std::string_view, WireError> parse_token_name(std::string_view body,
                                                                          std::int64_t index);

// One tradable spot pair with BOTH its token names and the BASE token's
// szDecimals already resolved. The pair row names its tokens by INDEX and the
// size precision lives on the token, so a mapper handed only the pair would
// have to guess the step.
//
// `name` is the wire coin, and it is NOT the pair's human name: every pair but
// the canonical PURR/USDC is spelled "@<index>" everywhere the venue mentions
// it — l2Book, openOrders, userFills. Using "HYPE/USDC" as a key finds nothing.
struct WireSpotAsset {
    std::string_view name;
    std::string_view base; // base token name
    // Quote token name, and it is NOT always USDC: 17 of 326 pairs settle in
    // USDH, USDT0 or USDE, and twelve of those bases ALSO have a USDC pair.
    // Dropping this field names four separate HYPE books "HYPE/USDC".
    std::string_view quote;
    std::int64_t index{0};       // pair index; the ASSET id for orders is 10000 + this
    std::int64_t sz_decimals{0}; // the BASE token's; both steps derive from it
};

// 326 pairs today and a deployer adds one whenever it likes.
inline constexpr std::size_t k_max_spot_assets = 1024;

struct SpotAssetBuffer {
    std::array<WireSpotAsset, k_max_spot_assets> assets{};
};

// Reads a {"type":"spotMeta"} body, joining universe[] to tokens[] by index —
// BOTH members, because the pair's canonical name needs both. A pair naming a
// token the response did not carry is DROPPED rather than loaded at a guessed
// precision under a name that cannot say which market it is.
[[nodiscard]] std::expected<std::span<const WireSpotAsset>, WireError>
parse_spot_meta(std::string_view body, SpotAssetBuffer& storage);

} // namespace raikiri::venues::hyperliquid
