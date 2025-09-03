#pragma once

// Hyperliquid wire types and parsers.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

using WireError = wire::Error;

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

// ---------------------------------------------------------- spot rest ----

// One tradable spot pair with the BASE token's szDecimals already resolved.
// The pair row names its tokens by INDEX and the size precision lives on the
// token, so a mapper handed only the pair would have to guess the step.
//
// `name` is the wire coin, and it is NOT the pair's human name: every pair but
// the canonical PURR/USDC is spelled "@<index>" everywhere the venue mentions
// it — l2Book, openOrders, userFills. Using "HYPE/USDC" as a key finds nothing.
struct WireSpotAsset {
    std::string_view name;
    std::string_view base; // base token name, for the local symbol
    std::int64_t index{0}; // pair index; the ASSET id for orders is 10000 + this
    std::int64_t sz_decimals{0};
};

// 326 pairs today and a deployer adds one whenever it likes.
inline constexpr std::size_t k_max_spot_assets = 1024;

struct SpotAssetBuffer {
    std::array<WireSpotAsset, k_max_spot_assets> assets{};
};

// Reads a {"type":"spotMeta"} body, joining universe[] to tokens[] by index.
// A pair naming a token the response did not carry is DROPPED rather than
// loaded at a guessed precision.
[[nodiscard]] std::expected<std::span<const WireSpotAsset>, WireError>
parse_spot_meta(std::string_view body, SpotAssetBuffer& storage);

// ------------------------------------------------------ private read ----

// A row of clearinghouseState.assetPositions[].position.
//
// `szi` is SIGNED — positive long, negative short — and there is no side
// field to disagree with it. It is also serialized from a float: a real
// response carries "-4216984.4000000004" for a position whose lot size is
// 0.1, so a reader that demands an exact multiple of the step will refuse
// rows the venue considers ordinary.
struct WirePosition {
    std::string_view coin;
    std::string_view size;     // szi
    std::string_view entry_px; // a weighted average, not a book price
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
    // clearinghouseState.time — the instant the whole snapshot describes.
    // There is no per-position timestamp, so every row shares this one.
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

// A row of openOrders, or of frontendOpenOrders — which is the same row plus
// the fields marked below. The plain form leaves them empty/false, and a
// caller that needs to know whether a resting order is post-only must ask the
// frontend form: `tif` is the only place "Alo" appears.
struct WireOpenOrder {
    std::string_view coin;
    std::string_view price;      // limitPx
    std::string_view qty;        // sz — REMAINING, not original
    std::string_view orig_qty;   // origSz
    std::string_view cloid;      // absent entirely on an order placed without one
    std::string_view order_type; // frontend only: "Limit", "Stop Market", ...
    std::string_view tif;        // frontend only: "Gtc" | "Alo" | "Ioc" | "FrontendMarket"
    std::uint64_t oid{0};
    std::uint64_t ts_ms{0};
    bool buy{false};         // `side`: "B" bids, "A" asks
    bool reduce_only{false}; // frontend only
    bool is_trigger{false};  // frontend only
};

inline constexpr std::size_t k_max_open_orders = 1024;

struct OpenOrderBuffer {
    std::array<WireOpenOrder, k_max_open_orders> orders{};
};

// Both forms parse here: the extra fields are read when present. The body is a
// bare ARRAY, not an envelope.
[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view body, OpenOrderBuffer& storage);

// A row of userFills / userFillsByTime, newest first.
//
// `crossed` is the liquidity flag: true means we took. `fee` is NEGATIVE when
// the venue paid us a maker rebate, so it must not be read as an unsigned
// magnitude.
struct WireFill {
    std::string_view coin;
    std::string_view price; // px
    std::string_view qty;   // sz
    std::string_view fee;
    std::string_view fee_token;
    std::string_view cloid; // absent on a fill of an order placed without one
    std::uint64_t oid{0};
    std::uint64_t tid{0};
    std::uint64_t ts_ms{0};
    bool buy{false};
    bool crossed{false};
};

// The venue caps a userFills answer at 2000 rows.
inline constexpr std::size_t k_max_fills = 2048;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

// {"status":"order","order":{"order":{...},"status":"filled","statusTimestamp":..}}
// or the bare {"status":"unknownOid"} — VERIFIED live, and the reason `found`
// exists: an order id the venue has forgotten and one it never minted answer
// identically, and neither is an error.
struct WireOrderStatus {
    WireOpenOrder order{};
    std::string_view status; // "open" | "filled" | "canceled" | "triggered" | ...
    std::uint64_t ts_ms{0};
    bool found{false};
};

[[nodiscard]] std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body);

// ---------------------------------------------------------- exchange ----

// What one entry of an /exchange reply says happened to one order.
enum class ExchangeOutcome : std::uint8_t {
    Resting,           // accepted and in the book
    Filled,            // matched on arrival, wholly or partly
    Success,           // a cancel or modify the venue performed
    WaitingForTrigger, // a conditional order armed; arrives as a bare STRING
    Error,             // per-order refusal, `error` carries the prose
    Unknown,
};

struct WireExchangeStatus {
    ExchangeOutcome outcome{ExchangeOutcome::Unknown};
    std::string_view error;     // prose; the protocol has no numeric code
    std::string_view avg_price; // filled only
    std::string_view total_qty; // filled only
    std::uint64_t oid{0};
};

// One action per request today; the array exists because the venue's shape is
// a batch and a future batch must not silently keep only its first answer.
inline constexpr std::size_t k_max_statuses = 64;

struct ExchangeStatusBuffer {
    std::array<WireExchangeStatus, k_max_statuses> statuses{};
};

// A REFUSED order is `status: "ok"` with an `error` inside `statuses` — only a
// malformed or unauthorised REQUEST reaches `status: "err"`. Reading the outer
// status alone therefore reports every rejected order as accepted.
//
// `response` is the same key with two types: an OBJECT on success and a plain
// STRING carrying prose on a top-level failure.
struct WireExchangeReply {
    bool ok{false};
    std::string_view message; // top-level prose, when !ok
    std::string_view type;    // "order" | "cancel" | "default"
    std::span<const WireExchangeStatus> statuses;
};

[[nodiscard]] std::expected<WireExchangeReply, WireError>
parse_exchange_reply(std::string_view body, ExchangeStatusBuffer& storage);

} // namespace raikiri::venues::hyperliquid
