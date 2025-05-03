#pragma once

// Bitfinex v2 wire types and parsers.
//
//   1. Arity is declared once per schema as `k_*_arity` beside the layout
//      comment, and `detail::read_row<Arity>` refuses a shorter array outright
//      (WireError::ShortArray). LENGTH IS THE ONLY STRUCTURAL CHECK THE
//      FORMAT OFFERS — a venue that INSERTS a field mid-row keeps the count
//      plausible and silently renames every later index, and no parser can
//      see that. What length does catch is truncation and appends.
//   2. Every index is a named `k_*` constant below, never a literal at the
//      read site, and `Row<Arity>::at<I>()` static_asserts I < Arity, so a
//      read past a declared schema does not compile.
//   3. map.cpp asserts semantic invariants a one-slot shift would break
//      (bid < ask, sizes positive) — those, not the parser, are what would
//      catch an insertion.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

using WireError = wire::Error;

// What a socket frame is, decided from its first two elements (data frames)
// or its `event` field (control frames). A data frame names no instrument at
// all — only its channel id — which is why the venue layer keeps a
// chanId -> instrument registry and this enum stops at "Data".
enum class FrameKind : std::uint8_t {
    Info,         // {"event":"info",...}: greeting, or a coded venue notice
    Subscribed,   // carries the chanId every later frame is keyed by
    Unsubscribed, //
    Error,        // {"event":"error","code":10300,...}
    Pong,         //
    Heartbeat,    // [CHAN_ID,"hb"] — NOT a payload; publishing it is garbage
    Data,         // [CHAN_ID,<payload>] or [CHAN_ID,"te",<payload>]
    Unknown,
};

struct FrameInfo {
    FrameKind kind{FrameKind::Unknown};
    std::int64_t chan_id{0}; // Data and Heartbeat only
    std::int64_t code{0};    // Info and Error; zero on the plain greeting
    std::string_view tag;    // "te"/"tu" on trade updates, empty otherwise
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<FrameInfo, WireError> parse_frame(std::string_view frame);

// Coded `info` notices. 20051 asks for a reconnect and 20061 ends a
// maintenance window; both REISSUE channel ids, which is why the venue layer
// treats them like a socket open rather than like a log line.
inline constexpr std::int64_t k_info_reconnect = 20051;
inline constexpr std::int64_t k_info_maintenance_start = 20060;
inline constexpr std::int64_t k_info_maintenance_end = 20061;

// The subscribe acknowledgement: the ONLY place a channel id is tied to an
// instrument. `symbol` names the pair on ticker/trades/book; the `status`
// channel names it inside `key` ("deriv:tBTCF0:USTF0") and carries no
// `symbol` at all, so both are surfaced.
struct WireSubscribed {
    std::int64_t chan_id{0};
    std::string_view channel; // ticker | trades | book | status
    std::string_view symbol;  // "tBTCUSD"; empty on the status channel
    std::string_view key;     // "deriv:tBTCF0:USTF0"; empty elsewhere
};

[[nodiscard]] std::expected<WireSubscribed, WireError> parse_subscribed(std::string_view frame);

// {"event":"error","code":10300,"msg":"symbol: invalid"} — code is a JSON
// NUMBER here, unlike the digit strings okx and bybit use.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// ticker payload: [BID,BID_SIZE,ASK,ASK_SIZE,DAILY_CHANGE,DAILY_CHANGE_REL,
//                  LAST_PRICE,VOLUME,HIGH,LOW]
//
// Bitfinex documents ten fields and sends ELEVEN on both spot and the
// perpetual (verified live 2026-08-11); the extra trailing element is `null`
// over the websocket and an unexplained integer over REST. The arity below is
// therefore what is READ, not what arrives — requiring the documented ten
// exactly would have been correct and would still have refused every live
// frame. Tests pin the observed count so a further append is visible.
inline constexpr std::size_t k_ticker_arity = 10;
inline constexpr std::size_t k_ticker_bid = 0;
inline constexpr std::size_t k_ticker_bid_size = 1;
inline constexpr std::size_t k_ticker_ask = 2;
inline constexpr std::size_t k_ticker_ask_size = 3;
inline constexpr std::size_t k_ticker_last_price = 6;

// No timestamp anywhere in the payload, so the mapper has only ingest time.
struct WireTicker {
    std::string_view bid;
    std::string_view bid_size;
    std::string_view ask;
    std::string_view ask_size;
    std::string_view last_price;
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// trade payload: [ID,MTS,AMOUNT,PRICE]
//
// AMOUNT IS SIGNED AND THE SIGN IS THE AGGRESSOR: positive means the taker
// bought. There is no side field to cross-check it against. Verified live —
// in one capture every negative print landed on the bid (63945/63951 against
// a 63951 bid) and every positive one at or above the ask. Read backwards,
// signed flow comes out exactly inverted and nothing errors.
inline constexpr std::size_t k_trade_arity = 4;
inline constexpr std::size_t k_trade_id = 0;
inline constexpr std::size_t k_trade_mts = 1;
inline constexpr std::size_t k_trade_amount = 2;
inline constexpr std::size_t k_trade_price = 3;

struct WireTrade {
    std::string_view id;
    std::string_view amount; // signed; see above
    std::string_view price;
    std::uint64_t ts_ms{0};
};

// A subscribe snapshot carries thirty prints; the ceiling leaves room without
// letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// `te` and `tu` are the SAME print twice: Bitfinex emits the execution
// immediately and restates it once the trade id is final. Both were observed
// carrying identical id/mts/amount/price, so a consumer that publishes both
// double-counts every trade on the venue.
enum class TradeFrameKind : std::uint8_t {
    Snapshot, // the array-of-arrays that follows a subscribe: history
    Executed, // "te"
    Updated,  // "tu"
};

struct WireTrades {
    TradeFrameKind kind{TradeFrameKind::Executed};
    std::span<const WireTrade> trades;
};

[[nodiscard]] std::expected<WireTrades, WireError> parse_trades(std::string_view frame,
                                                                TradeBuffer& storage);

// book level: [PRICE,COUNT,AMOUNT]
//
// COUNT == 0 removes the level. AMOUNT's SIGN IS THE SIDE here — positive is
// a bid, negative an ask — which is a different meaning from the same field
// on the trades channel, where the sign is the aggressor. On a delete the
// amount degenerates to +1/-1 and carries only the side.
inline constexpr std::size_t k_level_arity = 3;
inline constexpr std::size_t k_level_price = 0;
inline constexpr std::size_t k_level_count = 1;
inline constexpr std::size_t k_level_amount = 2;

struct WireLevel {
    std::string_view price;
    std::string_view amount;
    std::int64_t count{0};
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> levels{};
};

struct WireBook {
    bool snapshot{false};
    std::span<const WireLevel> levels;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

// deriv status payload, 23 elements of which fifteen are `null` placeholders.
// This is the funding/mark channel for perpetuals, subscribed by
// `key: "deriv:tBTCF0:USTF0"`.
//
// MARK_PRICE (14) and DERIV_PRICE (2) are different numbers and confusing them
// is the expensive mistake: DERIV_PRICE is the perpetual's own mid (63969.45
// against a 63962/63972 book) while MARK_PRICE tracks the index (63939.37
// against a 63938 spot) and is what liquidations use.
inline constexpr std::size_t k_deriv_arity = 23;
inline constexpr std::size_t k_deriv_mts = 0;
inline constexpr std::size_t k_deriv_price = 2;
inline constexpr std::size_t k_deriv_spot_price = 3;
inline constexpr std::size_t k_deriv_next_funding_mts = 7;
inline constexpr std::size_t k_deriv_next_funding_accrued = 8;
inline constexpr std::size_t k_deriv_next_funding_step = 9;
inline constexpr std::size_t k_deriv_current_funding = 11;
inline constexpr std::size_t k_deriv_mark_price = 14;
inline constexpr std::size_t k_deriv_open_interest = 17;

struct WireDerivStatus {
    std::string_view deriv_price;
    std::string_view spot_price;
    std::string_view mark_price;
    std::string_view current_funding;      // rate being applied now
    std::string_view next_funding_accrued; // running estimate for the next event
    std::string_view open_interest;
    std::uint64_t ts_ms{0};
    std::uint64_t next_funding_ms{0};
};

[[nodiscard]] std::expected<WireDerivStatus, WireError> parse_deriv_status(std::string_view frame);

// ------------------------------------------------------------- rest ----

// `["error",10020,"symbol: invalid"]`. An array, like everything else here,
// so an error body and a data body are told apart by their first element
// rather than by an envelope field.
inline constexpr std::size_t k_rest_error_arity = 3;
inline constexpr std::size_t k_rest_error_tag = 0;
inline constexpr std::size_t k_rest_error_code = 1;
inline constexpr std::size_t k_rest_error_msg = 2;

struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// Cheap enough to run on every body before parsing it as data: an error is
// the only Bitfinex REST answer whose first element is a string.
[[nodiscard]] bool is_rest_error(std::string_view body) noexcept;

[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// `GET /v2/conf/pub:info:pair` answers `[[[PAIR,[..]],...]]` — three deep,
// because /conf takes a list of keys and returns one result per key.
//
// The inner row is
//   [null,null,null,MIN_ORDER_SIZE,MAX_ORDER_SIZE,null,null,null,
//    INITIAL_MARGIN,MIN_MARGIN,(null,null)]
// with arity 12 on spot and 10 on futures, and MIN/MAX_ORDER_SIZE arriving as
// STRINGS while the margin fields beside them are bare numbers. Mixed types in
// one positional array; the raw-token reader is indifferent to which.
//
// NOTE WHAT IS ABSENT: no price tick, no amount step, no status field. See
// map.hpp — the steps are derived, and there is nothing to filter on but the
// symbol itself.
inline constexpr std::size_t k_pair_row_arity = 5;
inline constexpr std::size_t k_pair_min_order_size = 3;
inline constexpr std::size_t k_pair_max_order_size = 4;

struct WirePairInfo {
    std::string_view pair; // "BTCUSD" — NO `t` prefix on this endpoint
    std::string_view min_order_size;
    std::string_view max_order_size;
};

inline constexpr std::size_t k_max_pairs = 1024;

struct PairBuffer {
    std::array<WirePairInfo, k_max_pairs> pairs{};
};

[[nodiscard]] std::expected<std::span<const WirePairInfo>, WireError>
parse_pair_info(std::string_view body, PairBuffer& storage);

// `GET /v2/platform/status` answers `[1]`; zero is maintenance.
inline constexpr std::int64_t k_platform_operative = 1;

[[nodiscard]] std::expected<std::int64_t, WireError> parse_platform_status(std::string_view body);

} // namespace raikiri::venues::bitfinex
