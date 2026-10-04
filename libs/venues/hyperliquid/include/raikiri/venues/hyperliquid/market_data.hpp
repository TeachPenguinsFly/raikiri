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

// `side` "B" or "A" names the aggressor; no flip needed
struct WireTrade {
    std::string_view coin;
    std::string_view trade_id; // `tid`, a JSON number; kept as its raw digits
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

// `funding` is an hourly rate; there is no timestamp, so ts_event falls back to ingest
struct WireAssetCtx {
    std::string_view coin{};
    std::string_view funding{};
    std::string_view mark_price{};
    std::string_view mid_price{};
    std::string_view oracle_price{};
    std::string_view prev_day_price{};
    std::string_view open_interest{};
    std::string_view day_base_volume{};     // base units
    std::string_view day_notional_volume{}; // quote units
};

[[nodiscard]] std::expected<WireAssetCtx, WireError> parse_asset_ctx(std::string_view frame);

// must hold k_max_assets rows since the rows are positional against meta.universe
inline constexpr std::size_t k_max_asset_ctxs = 512;

struct AssetCtxBuffer {
    std::array<WireAssetCtx, k_max_asset_ctxs> ctxs{};
};

// metaAndAssetCtxs; rows carry no coin, so each takes the name at its position in the same body's universe
[[nodiscard]] std::expected<std::span<const WireAssetCtx>, WireError> parse_asset_ctxs(std::string_view body,
                                                                                       AssetCtxBuffer& storage);

} // namespace raikiri::venues::hyperliquid
