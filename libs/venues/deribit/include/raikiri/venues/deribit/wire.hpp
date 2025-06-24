#pragma once

// Deribit v2 wire types and parsers.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

using WireError = wire::Error;

// Resolved from the channel prefix once, so dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Bbo,       // book.{instrument}.none.1.100ms — depth-1 grouped snapshot
    Trade,     // trades.{instrument}.100ms
    Ticker,    // ticker.{instrument}.100ms — mark, index and funding in one
    Book,      // book.{instrument}.100ms — incremental, change_id chained
    UserChange, // user.changes.{kind}.{currency}.raw — private, authenticated
    Heartbeat, // server liveness probe; a test_request needs an answer
    Response,  // reply to something we sent: result or error
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view instrument;  // second channel segment; empty on replies
    bool needs_test_reply{false}; // Heartbeat only: type == "test_request"
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// ------------------------------------------------------------ public ----

struct WireBbo {
    std::string_view instrument;
    std::string_view bid_price; // raw JSON number tokens, exponents included
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::int64_t change_id{0};
    std::uint64_t ts_ms{0};
};

// Grouped, depth-limited books are always full snapshots — no type field, no
// prev_change_id — which is what makes this a BBO stream.
[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// `direction` is already the AGGRESSOR's side, so nothing is inverted here or
// in the mapper. `amount` is the venue's own quantity unit — USD on the
// reversed futures, coin contracts on options.
struct WireTrade {
    std::string_view instrument;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty; // `amount`
    bool aggressor_bought{false};
    std::int64_t trade_seq{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// One frame carries mark price and funding, so unlike OKX there is no separate
// mark channel to correlate. current_funding is the instantaneous rate and is
// legitimately 0.0 (captured live), so absence must be decided by the FIELD
// being missing — a dated future ships no funding fields at all. funding_8h is
// the trailing realised average: read so a schema change stays visible, but
// deliberately not mapped.
struct WireTicker {
    std::string_view instrument;
    std::string_view mark_price;
    std::string_view current_funding; // empty on anything but a perpetual
    std::string_view funding_8h;      // empty on anything but a perpetual
    // Empty on anything but an option. Deribit computes these itself and ships
    // them on the same ticker as the mark, so an options desk gets the venue's
    // own analytics rather than ours — see OptionGreeks for why that matters.
    // The implied vols arrive as PERCENTS here; the conversion to a ratio is
    // the mapper's, at the one edge where units change.
    std::string_view delta;
    std::string_view gamma;
    std::string_view vega;
    std::string_view theta;
    std::string_view rho;
    std::string_view mark_iv;
    std::string_view bid_iv;
    std::string_view ask_iv;
    std::string_view underlying_price;
    std::uint64_t ts_ms{0};
};

// True when the frame carried an option's analytics. A perpetual's ticker has
// no `greeks` object at all, so this separates "not an option" from "an option
// whose greeks are genuinely zero" — deep in the money, they are.
[[nodiscard]] constexpr bool has_greeks(const WireTicker& wire) noexcept {
    return !wire.delta.empty() || !wire.mark_iv.empty();
}

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// Deribit states the ACTION per level instead of leaving a removal to be
// inferred from its quantity. MEASURED 2026-08-17 across 904 deletes on three
// perpetuals: every one ALSO carried 0.0, so the two readings happen to agree
// — but the action is what the venue defines, so it is what the book keys on.
enum class LevelAction : std::uint8_t { New, Change, Delete };

struct WireLevel {
    LevelAction action{LevelAction::New};
    std::string_view price;
    std::string_view qty;
};

// A live BTC-PERPETUAL snapshot ran 897 bids / 1080 asks. Past this a frame is
// REFUSED, not truncated — half a book is worse than no book.
inline constexpr std::size_t k_max_levels = 4096;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the change_id chain: a delta applies when its prev_change_id
// equals the previous message's change_id. A snapshot carries none at all.
struct WireBook {
    std::string_view instrument;
    bool snapshot{false};
    std::int64_t change_id{0};
    std::int64_t prev_change_id{0}; // 0 on a snapshot: the field is absent
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

// A subscribe reply is {"id":N,"result":[<accepted channels>]}. Declined
// channels are SILENTLY ABSENT from that array (verified: an unknown
// instrument yields "result":[] with no error), so the accepted count is the
// only evidence a subscription took. Non-array results count zero.
// Deribit uses both negative (JSON-RPC standard) and positive codes, so the
// "no error" sentinel cannot be 0 the way an OKX envelope's is.
inline constexpr std::int64_t k_no_error = std::numeric_limits<std::int64_t>::min();

struct WireResponse {
    std::int64_t id{0};
    std::int64_t error_code{k_no_error};
    std::string_view error_message;
    std::size_t accepted{0};

    [[nodiscard]] bool failed() const noexcept { return error_code != k_no_error; }
};

// REST answers with the identical envelope, so this one reader serves both
// transports; absence of `error` is success.
[[nodiscard]] std::expected<WireResponse, WireError> parse_response(std::string_view frame);

// ------------------------------------------------------------- rest ----

// One /public/get_instruments row; numeric fields are raw tokens.
//
// kind + settlement_period together decide the market: BTC-PERPETUAL is kind
// "future" with settlement_period "perpetual", because there is no "perpetual"
// kind — asking for one is an Invalid params error (verified). instrument_type
// decides what contract_size COUNTS, which is the difference between a correct
// notional and one off by the price of BTC.
struct WireInstrument {
    std::string_view instrument_name;
    std::string_view kind;              // future, option, spot
    std::string_view instrument_type;   // reversed, linear
    std::string_view settlement_period; // perpetual, day, week, month
    std::string_view option_type;       // call, put; empty off options
    std::string_view tick_size;
    std::string_view contract_size;
    std::string_view min_trade_amount;
    std::string_view strike; // empty off options
    std::uint64_t expiration_ms{0};
    bool is_active{false};
};

// 794 live BTC options the day this was written. The catalogue is pulled one
// currency at a time so this tracks a strike ladder, not the whole universe.
inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

// /public/get_time answers a bare number as `result`, not an object.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// ---------------------------------------------------------- private ----

// One order row. Deribit returns the IDENTICAL object from private/buy,
// private/sell, private/edit, private/cancel, get_open_orders_by_currency,
// get_order_state_by_label and the user.orders channel, so one reader serves
// all seven and a schema drift shows up in every one of them at once.
//
// `amount` and `filled_amount` are in the unit the NAMED instrument is sized
// in — USD on a reversed future, coin on an option — which is exactly what
// the instrument's lot_size already divides by. Leaves is the difference, and
// it is computed at the mapping edge rather than trusted from a field Deribit
// does not publish.
struct WireOrder {
    std::string_view order_id;
    std::string_view label; // the ULID this adapter sent as the client id
    std::string_view instrument;
    std::string_view state; // open, filled, rejected, cancelled, (un)triggered
    // Deribit has no "expired" state: an IOC or FOK remainder is reported
    // "cancelled" like any other, and only this field tells the two apart.
    std::string_view tif;
    // Empty when Deribit spelled it "market_price", which it does for an open
    // trigger market order. A price that is not a number is not a price.
    std::string_view price;
    std::string_view amount;
    std::string_view filled_amount;
    std::string_view average_price;
    bool sell{false};
    std::uint64_t ts_ms{0}; // last_update_timestamp
};

// A currency-wide open-order sweep on a busy options account is the widest
// read here; past this the frame is REFUSED rather than truncated, because a
// partial reconciliation is worse than a failed one.
inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// One private execution. `direction` is the USER's own side: the channel
// documents it plainly, and on the inline trades private/buy returns the user
// is the aggressor anyway, so the taker reading and the user reading coincide
// exactly where the two spellings disagree.
struct WireUserTrade {
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view label;
    std::string_view instrument;
    std::string_view price;
    std::string_view amount;
    std::string_view fee;          // in fee_currency, which is the base coin
    std::string_view fee_currency;
    std::string_view state;        // the ORDER's state after this trade
    bool sell{false};
    bool maker{false};             // liquidity: "M" maker, "T" taker
    bool liquidity_stated{false};  // absent on some historical rows
    std::uint64_t ts_ms{0};
};

// get_user_trades_by_currency caps `count` at 1000; this adapter asks for a
// quarter of that per page so one pull cannot cost a quarter megabyte of
// scratch.
inline constexpr std::size_t k_max_user_trades = 256;

struct UserTradeBuffer {
    std::array<WireUserTrade, k_max_user_trades> trades{};
};

// private/buy and private/edit answer with the order AND the trades it caused
// in one envelope, which is the only place a fill and its order arrive
// together — the socket delivers them as two independent pushes.
struct WireOrderAck {
    WireOrder order{};
    std::span<const WireUserTrade> trades;
};

// `size` is SIGNED and denominated the same way `amount` is: positive long,
// negative short. average_price is the position's entry, in the instrument's
// own price unit.
struct WirePosition {
    std::string_view instrument;
    std::string_view size;
    std::string_view average_price;
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body,
                                                                     UserTradeBuffer& storage);

// `result` is an ARRAY on the list reads and a bare OBJECT on private/cancel;
// both carry the identical row, so both are accepted rather than one of them
// being a parse failure nobody notices until a cancel is in flight.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// One user.changes push: the orders that moved and the trades that moved them,
// in one frame. That pairing is the whole reason this channel is preferred
// over user.orders and user.trades — leaves is amount minus filled_amount off
// the SAME frame, never a guess stitched across two independent pushes.
struct WireChanges {
    std::span<const WireOrder> orders;
    std::span<const WireUserTrade> trades;
};

[[nodiscard]] std::expected<WireChanges, WireError>
parse_user_changes(std::string_view frame, OrderBuffer& orders, UserTradeBuffer& trades);

// get_user_trades_by_currency nests its array under result.trades beside a
// has_more flag; the socket puts the same array straight in params.data.
[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError>
parse_user_trades(std::string_view body, UserTradeBuffer& storage);

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::deribit
