#pragma once

// Hyperliquid wire types and parsers.
//
// Same layering rule as every adapter: DTOs mirror the venue's JSON exactly
// and a separate mapping step converts to domain types. Numbers stay as
// string_views into the caller's buffer.
//
// Four things differ from the CEX adapters in ways that reach this file. There
// are no REST GET endpoints for market data — every read is POST /info with a
// body naming the request type, so a REST parser's shape is chosen by that body
// rather than by a path. Timestamps are epoch milliseconds as JSON NUMBERS, so
// opt_ms reads a raw token where okx reads a quoted one. A frame is
// {"channel":..,"data":..} with the coin INSIDE data rather than beside the
// channel the way okx's `arg` does, so parse_event resolves only the kind and
// each parser reads its own coin. Asset names are bare tickers: "BTC".

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

enum class WireError : std::uint8_t {
    NotJson,
    MissingField,
    WrongType,
    TooManyEntries,
};

[[nodiscard]] std::string_view to_string(WireError err) noexcept;

// From `channel` once, so dispatch is an integer rather than a string compare.
enum class EventKind : std::uint8_t {
    Bbo,      // bbo — tick-by-tick top of book
    Trade,    // trades
    Book,     // l2Book — full depth SNAPSHOTS, never deltas
    AssetCtx, // activeAssetCtx — mark, funding, oracle and open interest
    SubscribeAck,
    Pong,
    Error,
    Unknown,
};

// No coin here: it sits inside `data` at a different depth per channel
// (`data.coin`, but `data[0].coin` on trades), so each parser reads its own.
[[nodiscard]] std::expected<EventKind, WireError> parse_event(std::string_view frame);

// {"channel":"error","data":"<prose>"} — `data` is a plain STRING and there is
// no numeric code anywhere in the protocol, so unlike okx's 60018 there is
// nothing to classify on but text. VERIFIED live: the venue also CLOSES the
// socket, so a bad subscription costs every other one on that connection.
struct StreamError {
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// {"px":..,"sz":..,"n":..}. `n` is an order count the domain does not model.
struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

struct WireBbo {
    std::string_view coin;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t ts_ms{0};
};

// `data.bbo` is [bid, ask] and either is null on a side with nothing resting;
// a null side yields empty views rather than an error, and the mapper drops it.
[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

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
struct WireAssetCtx {
    std::string_view coin; // empty on the REST rows: see parse_asset_ctxs
    std::string_view funding;
    std::string_view mark_price;
    std::string_view mid_price;
    std::string_view oracle_price;
    std::string_view prev_day_price;
    std::string_view open_interest;
};

[[nodiscard]] std::expected<WireAssetCtx, WireError> parse_asset_ctx(std::string_view frame);

// l2Book pushes 20 a side; headroom without letting a frame make us allocate.
inline constexpr std::size_t k_max_levels = 256;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// VERIFIED against consecutive live frames: every l2Book message is a full
// snapshot. No action field, no sequence id, no previous-sequence id anywhere
// in the payload — there is no continuity chain to maintain and no resync path
// to get wrong, the newest frame just replaces the book. `data.levels` is
// [bids, asks], bids descending and asks ascending.
struct WireBook {
    std::string_view coin;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

// ------------------------------------------------------------- rest ----

// A row of {"type":"meta"}. The venue publishes DECIMALS, not steps, and both
// steps are derived from szDecimals in the mapper. isDelisted is ABSENT on a
// live asset rather than false; a delisted row keeps its slot — 55 of 232.
struct WireAsset {
    std::string_view name;
    std::int64_t sz_decimals{0};
    std::int64_t max_leverage{0};
    bool delisted{false};
};

// 232 perps today, in one response; a builder-deployed DEX adds rows unasked.
inline constexpr std::size_t k_max_assets = 512;

struct AssetBuffer {
    std::array<WireAsset, k_max_assets> assets{};
};

// Reads the `universe` of a {"type":"meta"} body. A body of the bare literal
// `null` — which is how /info answers some malformed queries WITH HTTP 200 —
// fails here rather than yielding an empty catalogue.
[[nodiscard]] std::expected<std::span<const WireAsset>, WireError> parse_meta(std::string_view body,
                                                                              AssetBuffer& storage);

struct AssetCtxBuffer {
    std::array<WireAssetCtx, k_max_assets> ctxs{};
};

// The second element of a {"type":"metaAndAssetCtxs"} body, which ships the
// objects activeAssetCtx pushes. These rows carry NO coin — they are POSITIONAL
// against meta.universe — and a mismatched pairing would attach BTC's funding
// to another asset silently, so `coin` is left empty rather than guessed.
[[nodiscard]] std::expected<std::span<const WireAssetCtx>, WireError>
parse_asset_ctxs(std::string_view body, AssetCtxBuffer& storage);

} // namespace raikiri::venues::hyperliquid
