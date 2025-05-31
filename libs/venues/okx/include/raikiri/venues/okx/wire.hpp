#pragma once

// OKX v5 wire types and parsers.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

using WireError = wire::Error;

// Resolved from arg.channel once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Bbo,         // bbo-tbt — tick-by-tick top of book
    Trade,       // trades
    FundingRate, // funding-rate
    MarkPrice,   // mark-price
    Books,       // books — 400-level deltas
    Orders,      // orders — private, both lifecycle and fills
    SubscribeAck,
    LoginAck, // {"event":"login","code":"0"} — code 0 is the only success
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view inst_id; // empty on control frames
};

// A keepalive answer is the bare text "pong", not JSON, so it lands here as
// NotJson and the caller ignores it — the same path as any frame OKX adds.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription: {"event":"error","code":"60018","msg":"..."}. Its
// code is a string like every other OKX code.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

struct WireBbo {
    std::string_view inst_id;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::int64_t seq_id{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// `side` is already the AGGRESSOR's side — OKX states the taker direction
// directly, so nothing is inverted here or in the mapper. Binance's `m` says
// the opposite thing and needs a flip; reading these two the same way is how
// a signed flow series comes out backwards.
struct WireTrade {
    std::string_view inst_id;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t seq_id{0};
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

// funding_ms is OKX's `fundingTime`: the settlement the CURRENT rate applies
// to, which is the next settlement in wall-clock terms. `nextFundingTime` is
// the one after that, and reading it as "next funding" puts every countdown
// one interval into the future.
//
// The websocket funding-rate frame and GET /public/funding-rate ship the
// identical object, so one parser reads both.
struct WireFundingRate {
    std::string_view inst_id;
    std::string_view funding_rate;
    std::string_view next_funding_rate; // forecast; observed empty in practice
    std::uint64_t funding_ms{0};
    std::uint64_t next_funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// Also shared between the mark-price channel and GET /public/mark-price.
struct WireMarkPrice {
    std::string_view inst_id;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// `books` pushes 400 levels a side on the snapshot; the ceiling leaves room
// without letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the seqId chain: a delta applies when its prev_seq_id equals
// the previous message's seq_id. The checksum field OKX still pushes was
// DEPRECATED on 2026-06-23 and is now pinned to 0 — validating it would
// resync forever against a constant, which is why it is not surfaced at all.
struct WireBooks {
    std::string_view inst_id;
    bool snapshot{false}; // action: "snapshot" vs "update"
    std::int64_t seq_id{0};
    std::int64_t prev_seq_id{0}; // -1 on a snapshot
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBooks, WireError> parse_books(std::string_view frame,
                                                              LevelBuffer& storage);

// ----------------------------------------------------------- private ----

// OKX's order lifecycle. mmp_canceled is a market-maker-protection cancel:
// terminal, and not a fill however much the name reads like one.
enum class WireOrderState : std::uint8_t {
    Live,
    PartiallyFilled,
    Filled,
    Canceled,
    MmpCanceled,
    Unknown,
};

[[nodiscard]] WireOrderState order_state_from(std::string_view text) noexcept;

// One element of the private `orders` channel, which is also the shape
// GET /trade/orders-pending returns.
//
// An element is BOTH a fill and the state change that fill caused, and
// `trade_id` non-empty is OKX's own marker for "this update carries a
// trade". A reader that maps state only loses every fill; one that maps
// fills only loses every cancel. OKX documents that the fill_* fields carry
// DIFFERENT meanings on the REST order endpoints than on this channel, so
// nothing may read them off a REST body.
struct WireOrder {
    std::string_view inst_id;
    std::string_view ord_id;
    std::string_view cl_ord_id; // our ULID comes back here
    std::string_view side;      // "buy" | "sell"
    std::string_view ord_type;
    std::string_view px;
    std::string_view sz;          // the order's total size, never the remainder
    std::string_view acc_fill_sz; // cumulative; leaves = sz - acc_fill_sz
    std::string_view avg_px;
    std::string_view fill_px;
    std::string_view fill_sz;
    std::string_view fill_fee; // NEGATIVE is a fee charged, positive a rebate
    std::string_view trade_id;
    std::string_view exec_type;     // "T" taker, "M" maker
    std::string_view cancel_source; // "13"/"14": an FOK/IOC remainder expiring
    std::string_view amend_result;  // "-1": the venue refused an amend
    WireOrderState state{WireOrderState::Unknown};
    std::int64_t code{0}; // per-order failure; 0 on every healthy push
    std::uint64_t fill_ms{0};
    std::uint64_t u_ms{0};
};

// orders-pending pages at 100; the channel pushes one at a time.
inline constexpr std::size_t k_max_orders = 128;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// One element of GET /api/v5/trade/fills. `fee` here is the REST spelling of
// the channel's fill_fee and carries the same sign convention.
struct WireFill {
    std::string_view inst_id;
    std::string_view trade_id;
    std::string_view ord_id;
    std::string_view cl_ord_id;
    std::string_view side;
    std::string_view fill_px;
    std::string_view fill_sz;
    std::string_view fee;
    std::string_view exec_type;
    std::uint64_t fill_ms{0};
};

inline constexpr std::size_t k_max_fills = 128;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError>
parse_fills(std::string_view body, FillBuffer& storage);

// One element of GET /api/v5/account/positions. `pos` is signed in net mode
// and unsigned-per-side in long/short mode, so pos_side has to be read: a
// short read as a long is a position error of twice the size.
struct WirePosition {
    std::string_view inst_id;
    std::string_view pos;
    std::string_view pos_side; // "net" | "long" | "short"
    std::string_view avg_px;
    std::uint64_t u_ms{0};
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

// ------------------------------------------------------------- rest ----

inline constexpr std::int64_t k_ok = 0;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// Fails rather than reporting success when `code` is missing or is not a run
// of digits: a body whose envelope cannot be read must never be mistaken for
// a body that said "0".
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// One element of data[] on order / cancel-order / amend-order / the batch
// endpoints. The envelope's `code` reports only whether the BATCH went
// through: a single order OKX refuses answers code "1" — "Operation failed" —
// with the actual reason in sCode. A caller that reads only the envelope
// classifies every rejection as the same generic failure and can never tell
// "insufficient balance" from "post-only would have crossed".
struct WireOrderAck {
    std::string_view ord_id;
    std::string_view cl_ord_id;
    std::int64_t s_code{0};
    std::string_view s_msg;
};

// The batch endpoints cap at 20 entries; the ceiling leaves headroom.
inline constexpr std::size_t k_max_acks = 32;

struct AckBuffer {
    std::array<WireOrderAck, k_max_acks> acks{};
};

[[nodiscard]] std::expected<std::span<const WireOrderAck>, WireError>
parse_order_acks(std::string_view body, AckBuffer& storage);

// ct_val/ct_mult are empty on SPOT rows and on rows that are not live yet.
struct WireInstrument {
    std::string_view inst_id;
    std::string_view inst_type; // SWAP, FUTURES, SPOT, OPTION
    std::string_view state;     // live, preopen, suspend, expired
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view min_size;
    std::string_view ct_val;
    std::string_view ct_mult;
    std::string_view ct_type; // linear | inverse; empty on SPOT and OPTION rows
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

} // namespace raikiri::venues::okx
