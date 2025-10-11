#pragma once

// What the public feed says a market did: its prints, and the per-asset
// context that is this venue's whole answer to mark price, funding rate and
// the day's turnover at once.

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// `side` is "B" or "A" and names the AGGRESSOR: verified against a bbo capture
// taken on the same second, where every "A" print landed on the bid and every
// "B" print on the ask. Nothing is inverted here or in the mapper — binance's
// `m` says the opposite thing and needs a flip.
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

// One taker sweeping several makers arrives as one `data` array, hence a span.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// The per-asset context: this venue's answer to both "mark price" and "funding
// rate", which okx and binance split across two channels. `funding` is the
// HOURLY rate — funding settles every hour here, so comparing it unscaled
// against a CEX's 8h rate is a factor of eight, the most expensive misreading
// available on this venue. mid_price is null on a delisted asset (MATIC), and
// there is NO timestamp on either transport, so ts_event falls back to ingest.
//
// A SPOT pair's context is the same object minus the fields a spot market has
// no answer for — funding, open interest and the oracle price are absent, not
// zero — so the mapper declines to publish a rate for it and publishes only the
// turnover and the mark. It arrives on `activeSpotAssetCtx`, which is a channel
// name and not a second shape.
struct WireAssetCtx {
    std::string_view coin; // empty on the REST rows: see parse_asset_ctxs
    std::string_view funding;
    std::string_view mark_price;
    std::string_view mid_price;
    std::string_view oracle_price;
    std::string_view prev_day_price;
    std::string_view open_interest;
    // The rolling 24-hour turnover, on the SAME payload as funding: there is
    // no separate ticker to poll and no /info call in the path. dayBaseVlm is
    // the coin, dayNtlVlm the USDC notional — verified by ratio in map.cpp,
    // which is the check worth keeping because the names do not say which is
    // which. A delisted asset states "0.0" for both, which is a real zero.
    std::string_view day_base_volume;     // base units
    std::string_view day_notional_volume; // quote units
};

[[nodiscard]] std::expected<WireAssetCtx, WireError> parse_asset_ctx(std::string_view frame);

// MUST HOLD AS MANY ROWS AS THE CATALOGUE'S k_max_assets, because the rows are
// positional against meta.universe: raising one without the other truncates
// the ctxs against a universe that still fits. Its own constant all the same —
// sharing the catalogue's made a change to either resize the other silently.
inline constexpr std::size_t k_max_asset_ctxs = 512;

struct AssetCtxBuffer {
    std::array<WireAssetCtx, k_max_asset_ctxs> ctxs{};
};

// The second element of a {"type":"metaAndAssetCtxs"} body, which ships the
// objects activeAssetCtx pushes. These rows carry NO coin — they are POSITIONAL
// against meta.universe — and a mismatched pairing would attach BTC's funding
// to another asset silently, so `coin` is left empty rather than guessed.
[[nodiscard]] std::expected<std::span<const WireAssetCtx>, WireError>
parse_asset_ctxs(std::string_view body, AssetCtxBuffer& storage);

} // namespace raikiri::venues::hyperliquid
