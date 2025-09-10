#pragma once

// Gate.io v4 wire types and parsers.
//
// Timestamps come in both units and the field name does not settle it. `t` is
// milliseconds; `create_time` and `funding_next_apply` are seconds. Worse, the
// REST /futures/{settle}/trades endpoint ships `create_time_ms` as FRACTIONAL
// SECONDS (1786394934.376) where the websocket ships the same field name as
// integer milliseconds — verified live, 2026-08. Only the websocket form is
// parsed here.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

using WireError = wire::Error;

// Resolved from the frame's channel and event once, so downstream dispatch is
// an integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    BookTicker,   // {futures,spot}.book_ticker — top of book
    Trade,        // {futures,spot}.trades
    Ticker,       // futures.tickers — carries funding rate AND mark price
    BookUpdate,   // futures.order_book_update — depth deltas, needs a seed
    BookSnapshot, // spot.order_book — a complete book, no chain to keep
    OrderUpdate,  // futures.orders / spot.orders — private
    UserTrade,    // futures.usertrades / spot.usertrades — private
    SubscribeAck,
    Error,
    Pong,
    Unknown,
};

// `contract` is filled only on control frames, where it comes from the echoed
// `payload`. Data frames carry it in four different places depending on the
// channel, so their own parsers report it rather than paying for the descent
// twice.
struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view contract;
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription: the frame keeps event:"subscribe" and adds
// {"error":{"code":2,"message":"unknown currency pair NOPE_USDT"}}. Unlike the
// REST side the code here is a NUMBER. Code 4 nests a REST-style label inside
// the message string; classification still works off the number.
struct StreamError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// Sizes are integer CONTRACT counts, so bid_qty/ask_qty are digit tokens with
// no decimal point. `ts_ms` is milliseconds here even though most other Gate
// epoch fields are seconds.
struct WireBookTicker {
    std::string_view contract;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t update_id{0}; // `u`, exceeds 2^32 in practice
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame);

// `qty` is the MAGNITUDE of Gate's signed `size`; the sign became
// aggressor_bought. Reading `size` unsigned loses the taker direction, which
// is how a signed flow series comes out flat.
struct WireTrade {
    std::string_view contract;
    std::string_view qty;
    std::string_view price;
    std::int64_t trade_id{0};
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the other
// channels return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage, MarketType market);

// futures.tickers is the funding channel AND the mark-price channel: one
// subscription carries both, unlike okx. The interval is stated outright, so
// nothing has to be derived from two settlement times.
//
// `funding_rate_indicative` is DEPRECATED per Gate's own SDK ("use
// funding_rate") and was observed equal to it on every frame, so it is
// surfaced but deliberately not mapped to a forecast.
struct WireTicker {
    std::string_view contract;
    std::string_view funding_rate;
    std::string_view funding_rate_indicative;
    std::string_view mark_price;
    std::string_view index_price;
    // The base and quote columns by name. `volume_24h` alone is in
    // CONTRACTS on this venue, which compares with nothing.
    std::string_view volume_24h_base;
    std::string_view volume_24h_quote;
    std::uint64_t funding_next_apply_s{0};
    std::uint32_t funding_interval_s{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// A level is {"p":"64156.4","s":35680} on both the websocket and the REST
// snapshot — same object either way, unlike okx's positional arrays.
struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the [first_update_id, last_update_id] chain: a delta applies
// when its first_update_id is one past the previous message's last_update_id.
// Gate publishes no snapshot on this channel, so seeding it requires the REST
// order_book read that rest.hpp builds.
struct WireBookUpdate {
    std::string_view contract;
    std::uint64_t first_update_id{0}; // `U`
    std::uint64_t last_update_id{0};  // `u`
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBookUpdate, WireError> parse_book_update(std::string_view frame,
                                                                         LevelBuffer& storage);

// spot.order_book: a whole book, not a delta. Reported through the same type
// with both ids set to `lastUpdateId`, because nothing downstream chains it —
// the point of the snapshot channel is that there is no chain to break.
[[nodiscard]] std::expected<WireBookUpdate, WireError> parse_book_snapshot(std::string_view frame,
                                                                           LevelBuffer& storage);

// From GET /futures/{settle}/order_book, and only usable as a seed when that
// request carried `with_id=true`: without it the body has no `id`, and levels
// with no id are a picture of no particular point in the update chain.
// `update_s` is FRACTIONAL seconds (1786394534.46), truncated on the way in.
struct WireOrderBook {
    std::uint64_t update_id{0}; // `id`
    std::uint64_t update_s{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireOrderBook, WireError> parse_order_book(std::string_view body,
                                                                       LevelBuffer& storage);

// ------------------------------------------------------------- rest ----

// `message` is absent on some refusals (CONTRACT_NOT_FOUND ships the label
// alone), so only the label is required.
struct RestError {
    std::string_view label;
    std::string_view message;
};

// Fails when the body carries no `label`, which is how a success body reports
// itself. Callers gate this on a non-2xx status rather than probing every
// body: the catalogue is over a megabyte and indexing it twice is waste.
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// From GET /futures/{settle}/contracts. `order_size_min` is a count of
// contracts, and `quanto_multiplier` is how many base units one contract is.
struct WireContract {
    std::string_view name;
    std::string_view status; // prelaunch, trading, delisting, delisted, circuit_breaker
    std::string_view order_price_round;
    std::string_view order_size_min;
    std::string_view quanto_multiplier;
    std::string_view mark_price;
    std::string_view funding_rate;
    std::uint64_t funding_next_apply_s{0};
    std::uint32_t funding_interval_s{0};
    bool in_delisting{false};
};

// 899 live on 2026-08 and the count only grows; past this a response is a
// venue incident or an attempt to make us allocate.
inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

// The body is a top-level ARRAY, not an object with a data member.
[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// GET /spot/time — milliseconds as a JSON number; futures has no equivalent.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// From GET /spot/currency_pairs. Precisions are DIGIT COUNTS, not steps:
// `precision` is price decimals and `amount_precision` is quantity decimals,
// so the steps are 10^-n and a pair with amount_precision 0 trades in whole
// units.
struct WireCurrencyPair {
    std::string_view id;
    std::string_view trade_status; // tradable, untradable, buyable, sellable
    std::string_view min_base_amount;
    std::string_view min_quote_amount;
    std::uint32_t precision{0};
    std::uint32_t amount_precision{0};
};

// 2230 pairs live on 2026-08; the count only grows.
inline constexpr std::size_t k_max_currency_pairs = 8192;

struct CurrencyPairBuffer {
    std::array<WireCurrencyPair, k_max_currency_pairs> pairs{};
};

[[nodiscard]] std::expected<std::span<const WireCurrencyPair>, WireError>
parse_currency_pairs(std::string_view body, CurrencyPairBuffer& storage);

// ---------------------------------------------------------- private ----

// What an order frame says happened, normalised across the two APIs. Gate
// spells this three different ways — futures `finish_as` (with the WS-only
// pseudo-values `_new` and `_update`), spot `status`, and the spot socket's
// per-order `event` — and none of them is a superset of the others.
enum class WireOrderState : std::uint8_t {
    Open,      // accepted, still working
    Filled,    // finished, fully filled
    Cancelled, // finished by a cancel, a liquidation or self-trade prevention
    Expired,   // finished because ioc/poc/fok could not be met
    Unknown,
};

// One order across both APIs. `amount` and `left` are MAGNITUDES: futures
// signs both to carry the side, and a reader that keeps the sign turns a short
// into a negative quantity halfway down the mapper.
struct WireOrder {
    std::string_view contract;
    std::string_view order_id; // `id`, venue-minted
    std::string_view text;     // our "t-<ulid>" comes back here
    std::string_view price;
    std::string_view amount; // futures `size`, spot `amount`
    std::string_view left;
    std::string_view fill_price; // AVERAGE over the order, not this fill
    std::string_view tif;        // gtc, ioc, poc, fok
    std::string_view order_type; // spot only; futures has no type field
    bool sell{false};
    bool accepted{false}; // the frame that says the order reached the book
    WireOrderState state{WireOrderState::Unknown};
    std::uint64_t ts_ms{0};
};

// Gate's own cap is 1000 rows per list call, which is also the largest
// websocket batch observed.
inline constexpr std::size_t k_max_private_entries = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_private_entries> orders{};
};

// One fill. There is no `left` here on either API — the remaining size lives
// on the ORDERS channel — so a mapper needs both frames to state leaves.
struct WireUserTrade {
    std::string_view contract;
    std::string_view trade_id; // `id`, the dedupe key
    std::string_view order_id;
    std::string_view text;
    std::string_view price;
    std::string_view qty; // magnitude of futures `size` / spot `amount`
    std::string_view fee;
    bool sell{false};
    bool maker{false};
    std::uint64_t ts_ms{0};
};

struct UserTradeBuffer {
    std::array<WireUserTrade, k_max_private_entries> trades{};
};

// The futures account, one settle currency at a time. Gate answers this from
// the same endpoint the private socket's user id comes from — the numbers were
// already arriving and being thrown away.
struct WireFuturesAccount {
    std::string_view currency;
    std::string_view total;
    std::string_view available;
    std::string_view unrealised_pnl;
    std::uint64_t update_s{0};
};

struct WirePosition {
    std::string_view contract;
    std::string_view size; // SIGNED: negative is short
    std::string_view entry_price;
    std::uint64_t update_s{0};
};

struct PositionBuffer {
    std::array<WirePosition, k_max_private_entries> positions{};
};

// Websocket pushes: the rows hang off `result`, an array on both channels.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_ws_orders(std::string_view frame, OrderBuffer& storage, MarketType market);

[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError>
parse_ws_user_trades(std::string_view frame, UserTradeBuffer& storage, MarketType market);

// REST bodies: a top-level array, with no envelope to unwrap.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market);

[[nodiscard]] std::expected<WireOrder, WireError> parse_rest_order(std::string_view body,
                                                                   MarketType market);

// GET /spot/open_orders answers with one row PER CURRENCY PAIR, each holding
// its own `orders` array. Reading it as a flat order list yields nothing at
// all, silently.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_spot_open_orders(std::string_view body, OrderBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError>
parse_rest_user_trades(std::string_view body, UserTradeBuffer& storage, MarketType market);

// One row of the futures account book, narrowed to settlements.
//
// TIME IS IN SECONDS here, unlike the millisecond stamps gate's order
// endpoints use. `change` is signed the way the account moved, which is
// already the domain's convention.
struct WireAccountBookRow {
    std::string_view contract;
    std::string_view type; // fund | fee | dnw | pnl | ...
    std::string_view change;
    std::string_view text;
    // A STRING, not a number — gate quotes it as "991" while `time` on the
    // same row is a bare number. Verified against a live settlement row.
    std::string_view id;
    // The trade a `fee` row was charged on; empty on `fund` rows, which have
    // no trade. A STRING here too, like `id`.
    std::string_view trade_id;
    std::uint64_t time_s{0};
};

inline constexpr std::size_t k_max_book_rows = 256;

struct AccountBookBuffer {
    std::array<WireAccountBookRow, k_max_book_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireAccountBookRow>, WireError>
parse_account_book(std::string_view body, AccountBookBuffer& storage);

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

// `user` from GET /futures/{settle}/accounts, rendered as digits. The futures
// private socket wants it as the first element of every subscribe payload and
// answers a payload without it with an empty stream rather than an error.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_account_user_id(std::string_view body);

std::expected<WireFuturesAccount, WireError> parse_futures_account(std::string_view body);

} // namespace raikiri::venues::gate
