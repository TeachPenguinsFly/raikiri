#pragma once

// HTX (formerly Huobi) USDT-M swap wire types and parsers.
//
// Market data and funding arrive from two endpoints speaking two dialects —
// `ch`/`sub` on /linear-swap-ws, `op`/`topic` on /linear-swap-notification.
// One EventKind covers both, because `ch` and `op` never appear together.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

using WireError = wire::Error;

// Resolved once per frame, so downstream dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Bbo,             // market.{code}.bbo
    Trade,           // market.{code}.trade.detail
    Depth,           // market.{code}.depth.step0
    Detail,          // market.{code}.detail — the 24-hour window
    Funding,         // op:notify carrying public.{code}.funding_rate
    MarketPing,      // {"ping":<ms>}
    NotifyPing,      // {"op":"ping","ts":"<ms>"}
    SpotPing,        // {"action":"ping","data":{"ts":<ms>}} — the spot v2 dialect
    AuthReply,       // the answer to a signed handshake on either private socket
    PrivateOrder,    // orders_cross.{code} / orders#{symbol}
    PrivateMatch,    // trade.clearing#{symbol}#0; the swap folds fills into orders
    PrivatePosition, // positions_cross.{code}
    SubscribeAck,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view contract_code; // empty on control frames and keepalives

    std::string_view keepalive_ts;
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

struct StreamError {
    std::string_view code; // "bad-request" or "2011": see the header note
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// `bid`/`ask` are two-element arrays: price then a count of CONTRACTS, never
// base units. tick.ts is the matching-engine time; the root `ts` is when HTX
// sent the frame.
struct WireBbo {
    std::string_view contract_code;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t ts_ms{0};
    std::uint64_t version{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// `direction` is already the AGGRESSOR's side — HTX names the taker directly,
// so nothing is inverted here or in the mapper. bingx's `m` says the opposite
// thing, and reading the two the same way is how a signed flow series comes
// out backwards.
//
// `amount` is contracts and `quantity` the same trade in base units. Only
// amount is mapped: bbo and depth quote contracts too.
struct WireTrade {
    std::string_view contract_code;
    std::string_view price;
    std::string_view amount;
    std::string_view quantity;
    bool aggressor_bought{false};
    std::uint64_t trade_id{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the other
// channels return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// step0 is the unaggregated book and ships about 150 levels a side; the
// ceiling leaves room without letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// A complete snapshot every time, with no delta chain and so no continuity to
// validate. `version` equals tick.id on step0 and tick.mrid on the aggregated
// levels, which is why it is carried rather than trusted as a sequence.
//
// RE-VERIFIED 2026-08-17 on BTC-USDT: 119 consecutive step0 frames, every one
// of them 150 levels a side with no action field anywhere. `version` and `id`
// are epoch SECONDS, so ten frames a second share one value; `mrid` is the
// matching-engine id and is the only token here that orders frames.
struct WireDepth {
    std::string_view contract_code;
    std::uint64_t ts_ms{0};
    std::uint64_t version{0};
    std::uint64_t mrid{0};
    std::span<const WireLevel> bids; // descending
    std::span<const WireLevel> asks; // ascending
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

// market.{code}.detail — the rolling 24-hour window, and the only push HTX
// has for it: the funding notification states no turnover, and neither does
// bbo, trade.detail or depth.step0.
//
// THREE volume figures arrive on this tick and only two of them compare with
// anything. `amount` is the window in BASE units and `trade_turnover` the same
// window in QUOTE; `vol` is the same trade count in CONTRACTS — VERIFIED live
// on BTC-USDT, vol 8266024 against amount 8266.024 at a contract_size of
// 0.001 — and is deliberately not surfaced, because a contract is a different
// quantity on every row of the catalogue.
//
// A whole snapshot every push, not a delta. Both columns are optional anyway:
// empty is what a contract with nothing to report would leave here, and empty
// is not zero.
struct WireDetail {
    std::string_view contract_code;
    std::string_view amount;         // base units
    std::string_view trade_turnover; // quote units
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireDetail, WireError> parse_detail(std::string_view frame);

// The websocket funding notification. `settlement_time` is the next
// settlement; `funding_time` is when HTX built the message and moves every few
// seconds. GET /swap_funding_rate gives `funding_time` the OTHER meaning, so
// the two surfaces name different instants alike — hence only the socket shape
// is parsed here. The rate is a STRING of eighteen fractional digits whose
// tail is not always zero ("-0.000058784590419109" live on ETH-USDT).
struct WireFunding {
    std::string_view contract_code;
    std::string_view funding_rate;
    std::string_view estimated_rate; // JSON null in every frame observed
    std::uint64_t settlement_ms{0};
    std::uint64_t funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFunding, WireError> parse_funding(std::string_view frame);

// ------------------------------------------------------------- rest ----

// `status` is "ok" or "error"; there is no success code to compare against.
// A body that carries neither — HTX answers an unrouted path with a Spring
// error whose `status` is the NUMBER 404 — fails to parse rather than
// reporting success, so the caller falls back to the HTTP status.
struct RestEnvelope {
    bool ok{false};
    std::string_view code; // empty when the venue named none
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// contract_size is what one contract is worth in base units, and the only
// quantity unit HTX publishes: an order is an integer count of contracts, so
// there is no separate step. settlement_period is the funding interval in
// HOURS as a digit string ("8", "4" and "1" are all live), which okx has to
// infer from consecutive funding frames.
struct WireContract {
    std::string_view contract_code;
    std::string_view symbol;
    std::string_view contract_size;
    std::string_view price_tick;
    std::string_view business_type; // "swap" on every row this endpoint serves
    std::string_view contract_type;
    std::string_view delivery_time; // empty on a swap, an epoch on a delisting
    std::int64_t contract_status{0};
    std::int64_t settlement_period_h{0};
    std::uint64_t settlement_ms{0};
};

inline constexpr std::size_t k_max_instruments = 1024; // 276 rows live today

struct ContractBuffer {
    std::array<WireContract, k_max_instruments> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// GET /api/v1/timestamp answers {"status":"ok","ts":<ms>} with no `data` at
// all, so this reads the envelope's own timestamp.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// From GET /v1/common/symbols on api.huobi.pro. Precisions are DIGIT COUNTS,
// not steps: `price-precision` is price decimals and `amount-precision` is
// quantity decimals, so the steps are 10^-n. `api-trading` is a separate gate
// from `state` — a pair can be online for the web UI and closed to keys.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view state;       // online, offline, suspend, pre-online, transfer-board
    std::string_view api_trading; // "enabled" / "disabled"
    std::string_view min_order_value;
    std::uint32_t price_precision{0};
    std::uint32_t amount_precision{0};
};

// 2159 pairs live on 2026-08 and the count only grows.
inline constexpr std::size_t k_max_spot_symbols = 8192;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

// ---------------------------------------------------------- private ----

// The `id` of the first working account of type "spot" from
// GET /v1/account/accounts. Every spot order carries it, and there is no
// default: an order without account-id is refused as an invalid parameter.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_account_id(std::string_view body);

// What an order frame says the order is now. HTX spells this two ways — a
// swap `status` integer and a spot `orderStatus`/`state` word — and neither is
// a superset of the other.
enum class WireOrderState : std::uint8_t {
    Accepted, // reached the book; nothing filled yet
    PartiallyFilled,
    Filled,
    Canceled, // includes a partial fill that was then cancelled
    Rejected,
    Unknown,
};

// One fill. `fee` is what the venue charged for THIS trade, in the fee asset;
// HTX writes a charge as a NEGATIVE number on the swap API and a positive one
// on spot, so the mapper takes the magnitude rather than the sign.
struct WireFill {
    std::string_view exec_id; // swap trade `id`, spot `tradeId`
    std::string_view price;
    std::string_view qty;
    std::string_view fee; // empty when the frame carries none
    bool maker{false};
    std::uint64_t ts_ms{0};
};

// One order across both APIs. Sizes are CONTRACTS on the swap and base units
// on spot; the mapper divides by the instrument's own lot size either way.
//
// `left_qty` and `filled_qty` are the two spellings of the same fact and never
// both arrive: spot states what remains, the swap states what filled. A mapper
// that reads only one of them reports every order on the other surface as
// untouched.
struct WireOrder {
    std::string_view contract_code;
    std::string_view order_id;
    std::string_view client_order_id; // swap: digits; spot: our ULID
    std::string_view price;
    std::string_view qty;
    std::string_view filled_qty;       // swap trade_volume, spot execAmt
    std::string_view left_qty;         // spot remainAmt; empty on the swap
    std::string_view order_price_type; // swap order_price_type, spot `type`
    bool sell{false};
    WireOrderState state{WireOrderState::Unknown};
    std::uint64_t ts_ms{0};
    // Swap only, and only on the socket: the fills this frame reports. Spot
    // splits them onto trade.clearing, which is the only place its fee exists.
    std::span<const WireFill> fills;
};

// One fill with the identity of the order it belongs to: spot's trade.clearing
// push and both surfaces' REST match-result rows take this shape.
struct WireUserTrade {
    std::string_view contract_code;
    std::string_view order_id;
    std::string_view client_order_id;
    // Spot's trade.clearing states what the order has left; the swap's match
    // rows and spot's REST match rows do not, and the caller falls back to
    // what the order channel last said.
    std::string_view left_qty;
    WireFill fill;
    bool sell{false};
};

struct WirePosition {
    std::string_view contract_code;
    std::string_view volume; // MAGNITUDE in contracts; `sell` carries the sign
    std::string_view cost_hold;
    bool sell{false};
    std::uint64_t ts_ms{0};
};

// The swap sweeps page at 50 and spot's list endpoints cap at 500.
inline constexpr std::size_t k_max_private_entries = 512;

// One websocket order frame reports at most a handful of trades; the ceiling
// exists so a hostile frame cannot make us walk past the buffer.
inline constexpr std::size_t k_max_fills_per_frame = 64;

struct OrderBuffer {
    std::array<WireOrder, k_max_private_entries> orders{};
    std::array<WireFill, k_max_fills_per_frame> fills{};
};

struct PositionBuffer {
    std::array<WirePosition, k_max_private_entries> positions{};
};

// What a create answered with. Spot's `data` is the order id as a bare STRING;
// the swap's is an object carrying both ids, and its client_order_id is the
// echo that proves which order this is.
struct WireOrderAck {
    std::string_view order_id;
    std::string_view client_order_id;
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body,
                                                                     MarketType market);

// What a cancel answered with. The swap answers status "ok" whether the order
// was cancelled or not — the per-id verdict hides in `data.errors` — and spot's
// cancel-by-client-order-id answers a bare INTEGER state where 0 means the id
// was never found. Reading either as success by the envelope alone leaves a
// live order the OMS believes is gone.
struct WireCancelOutcome {
    bool accepted{false};
    std::string_view code; // the venue's own refusal code, when it named one
};

[[nodiscard]] std::expected<WireCancelOutcome, WireError>
parse_cancel_outcome(std::string_view body, MarketType market);

// Websocket pushes ------------------------------------------------------

// {"op":"notify","topic":"orders_cross.btc-usdt",...} — one order per frame,
// with its new trades inline.
[[nodiscard]] std::expected<WireOrder, WireError> parse_swap_order_push(std::string_view frame,
                                                                        OrderBuffer& storage);

// {"action":"push","ch":"orders#btcusdt","data":{...}} — one order per frame
// and never a fee, whatever the eventType.
[[nodiscard]] std::expected<WireOrder, WireError> parse_spot_order_push(std::string_view frame);

// {"ch":"trade.clearing#btcusdt#0","data":{...}} — the only spot frame that
// carries transactFee.
[[nodiscard]] std::expected<WireUserTrade, WireError> parse_spot_trade_push(std::string_view frame);

// The swap socket answers {"op":"auth","err-code":0,...}; the spot v2 socket
// answers {"action":"req","ch":"auth","code":200,...}. Both reduce to this.
struct WireAuthReply {
    bool ok{false};
    std::string_view code;
    std::string_view msg;
};

[[nodiscard]] std::expected<WireAuthReply, WireError> parse_auth_reply(std::string_view frame);

// REST bodies -----------------------------------------------------------

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market);

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::htx
