#pragma once

// KuCoin wire types and parsers, public market data and private order flow.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

using WireError = wire::Error;

// Resolved from type/subject once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Bbo,         // subject tickerV2
    Trade,       // subject match, on /contractMarket/execution
    Depth,       // subject level2
    MarkPrice,   // subject mark.index.price, on /contract/instrument
    FundingRate, // subject funding.rate, on the same topic
    OrderChange, // subject orderChange (or symbolOrderChange), private topic
    Welcome,     // sent once on connect, before anything may be subscribed
    Ack,
    Pong,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// The private topics carry no symbol behind their topic string — the payload
// names it instead — so the public discriminator, which reads the symbol out
// of the topic, refuses these frames outright.
[[nodiscard]] std::expected<EventInfo, WireError> parse_private_event(std::string_view frame);

// A refused subscription or a rejected token. `data` holds the text here,
// where a REST body would use `msg`.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// Sizes are CONTRACTS and arrive as bare integers; they still travel as text so
// the mapper divides by the instrument's lot step rather than assuming the
// lotSize of 1 that KuCoin happens to list on every contract today.
struct WireBbo {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// `side` is already the AGGRESSOR's side — KuCoin states the taker direction
// directly, so nothing is inverted here or in the mapper. Binance's `m` says the
// opposite thing, and reading the two alike is how signed flow comes out backwards.
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame);

// One level2 frame is ONE level packed into a comma-joined string:
// "62234.6,buy,1" is price, side, size, and a size of 0 deletes the level. The
// stream carries no snapshot; a book must start from GET /api/v1/level2/snapshot
// and replay from `sequence`.
struct WireBookChange {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool bid{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBookChange, WireError> parse_book_change(std::string_view frame);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// The websocket form carries no settlement time; the REST form does but names
// the funding INDEX (".XBTUSDTMFPI8H") in `symbol`, useless for lookup — hence
// one DTO, two parsers, and a `symbol` only the websocket form fills.
//
// `granularity` means DIFFERENT THINGS on the two transports, so the mapper
// trusts neither: the stream republishes the live rate once a minute and says
// 60000, while REST says the 28800000 settlement period (verified live).
struct WireFundingRate {
    std::string_view symbol;
    std::string_view rate;
    std::string_view predicted_rate; // REST `predictedValue`; absent in practice
    std::uint64_t granularity_ms{0};
    std::uint64_t next_funding_ms{0}; // REST `fundingTime`; 0 on the websocket
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// ------------------------------------------------------------- rest ----

// "200000" is success. The code is a string, so it is converted once here and
// everything below works in integers.
inline constexpr std::int64_t k_ok = 200'000;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// Fails when `code` is missing or is not a run of digits: a body whose envelope
// cannot be read must never be mistaken for one that said "200000".
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// POST /api/v1/bullet-public: a short-lived token plus the endpoint to use it
// against. Part of connecting rather than of startup, because it expires.
struct WireBullet {
    std::string_view token;
    std::string_view endpoint; // "wss://ws-api-futures.kucoin.com/"
    std::uint64_t ping_interval_ms{0};
    std::uint64_t ping_timeout_ms{0};
};

[[nodiscard]] std::expected<WireBullet, WireError> parse_bullet(std::string_view body);

// `multiplier` is how many base units one CONTRACT is worth, and it is negative
// on inverse contracts, whose size is quoted in the quote currency instead; the
// mapper refuses those rather than storing a negative size.
struct WireContract {
    std::string_view symbol;
    std::string_view status; // "Open"
    std::string_view type;   // FFWCSX perpetual, FFICSX dated
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view multiplier;
    std::string_view index_tick_size;
    std::string_view max_order_qty;
    std::uint64_t funding_granularity_ms{0};
};

inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// GET /api/v1/level2/snapshot. The only place a KuCoin book can start: the
// websocket carries no snapshot at all. Levels are [price, size] pairs of JSON
// NUMBERS, so the comma-joined string of the stream has no counterpart here.
// `sequence` is the last change already inside these levels, so the frame that
// carries them forward is the one holding sequence + 1.
struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// 1000 a side measured on XBTUSDTM; the ceiling leaves room without letting a
// body make us allocate.
inline constexpr std::size_t k_max_levels = 2048;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// `ts` here is NANOSECONDS, where every other KuCoin timestamp is milliseconds.
struct WireOrderBook {
    std::string_view symbol;
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
    std::span<const WireLevel> bids; // descending
    std::span<const WireLevel> asks; // ascending
};

[[nodiscard]] std::expected<WireOrderBook, WireError> parse_level2_snapshot(std::string_view body,
                                                                            LevelBuffer& storage);

// GET /api/v1/funding-rate/{symbol}/current. Seeds funding at subscribe time and
// is the only source that states the venue's own next settlement.
[[nodiscard]] std::expected<WireFundingRate, WireError>
parse_funding_current(std::string_view body);

// GET /api/v1/timestamp. `data` is a bare number here, not an object.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);


// ---------------------------------------------------------- private ----

// KuCoin puts the WHOLE order lifecycle on one topic per market family: there
// is no separate execution channel the way bybit has, and an ack, a fill and a
// cancel differ only by `type`.
//
// `Filled` is the venue saying the order is done; the trades that filled it
// already arrived as `Match`. Treating it as another fill double-counts the
// last one.
enum class OrderChangeType : std::uint8_t {
    Open,
    Match,
    Filled,
    Canceled,
    Update, // a modify took effect; `old_size` names what it replaced
    Unknown,
};

// Sizes are decimal text on spot and CONTRACTS on futures, and `order_time` is
// a JSON number on spot but a quoted string on futures — both reach the DTO as
// the raw token, so the mapper divides by the instrument's own steps either way.
struct WireOrderChange {
    OrderChangeType type{OrderChangeType::Unknown};
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view trade_id;  // `match` only; the fill's dedupe key
    std::string_view side;      // "buy" / "sell"
    std::string_view order_type; // "limit" / "market"
    std::string_view price;      // the order's price; "0" on a market order
    std::string_view size;
    std::string_view filled_size;
    std::string_view match_price; // `match` only, and the price that traded
    std::string_view match_size;  // `match` only
    std::string_view remain_size;
    std::string_view canceled_size;
    std::string_view old_size;    // `update` only
    std::string_view liquidity;   // "maker" / "taker"; futures states it, spot does not
    // `order_time` is MILLISECONDS on spot and NANOSECONDS on futures — the
    // same field name, two scales, six orders of magnitude apart. `ts` is
    // nanoseconds on both, so it is the one the mapper times events by.
    std::uint64_t order_time{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireOrderChange, WireError>
parse_order_change(std::string_view frame);

// ------------------------------------------------------ private rest ----

// One row shape for both hosts: the spot and futures order lists disagree on
// which fields they send, not on what the shared ones are called. `remain_size`
// is spot-only — futures states `deal_size` and leaves the subtraction to the
// mapper.
struct WireOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view side;
    std::string_view order_type;
    std::string_view price;
    std::string_view size;
    std::string_view deal_size;
    std::string_view remain_size;
    std::string_view status; // futures "open" / "done"; absent on spot
    bool active{false};
    bool cancel_exist{false};
    std::uint64_t updated_ms{0};
};

inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// GET /api/v1/hf/orders/active — `data` is the array itself.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_spot_orders(std::string_view body, OrderBuffer& storage);

// GET /api/v1/orders?status=active — `data` is a page object wrapping `items`.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_futures_orders(std::string_view body, OrderBuffer& storage);

inline constexpr std::size_t k_max_active_symbols = 256;

struct SymbolBuffer {
    std::array<std::string_view, k_max_active_symbols> symbols{};
};

// GET /api/v1/hf/orders/active/symbols. The first half of the spot sweep:
// KuCoin answers actives one symbol at a time.
[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_active_symbols(std::string_view body, SymbolBuffer& storage);

// Neither host names the clientOid on a fill, so a fill is attributed through
// its `order_id` alone.
struct WireFill {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view side;
    std::string_view liquidity; // "maker" / "taker"
    std::string_view price;
    std::string_view size;
    std::string_view fee;
    std::uint64_t created_ms{0};
};

inline constexpr std::size_t k_max_fills = 512;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

// GET /api/v1/fills and GET /api/v1/hf/fills — one shape, both hosts.
[[nodiscard]] std::expected<std::span<const WireFill>, WireError>
parse_fills(std::string_view body, FillBuffer& storage);

// `current_qty` is SIGNED and counted in contracts: negative is short, and
// there is no side field to disagree with it.
struct WirePosition {
    std::string_view symbol;
    std::string_view current_qty;
    std::string_view avg_entry_price;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

// GET /api/v1/positions — `data` is the array itself.
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

// GET /api/v2/symbols on the SPOT host. `base_increment` is the lot step and
// `price_increment` the tick; `min_funds` is a quote-currency floor, not a size.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view base_increment;
    std::string_view price_increment;
    std::string_view min_funds;
    bool enable_trading{false};
};

inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::kucoin
