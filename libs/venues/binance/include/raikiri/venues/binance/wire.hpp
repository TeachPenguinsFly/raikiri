#pragma once

// Binance USD-M futures wire types and parsers.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

using WireError = wire::Error;

// Resolved from the frame's event type once, so downstream dispatch is an
// integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    BookTicker,    // <sym>@bookTicker
    Trade,         // <sym>@trade, or <sym>@aggTrade
    MarkPrice,     // <sym>@markPrice — mark, index and funding in one frame
    DepthUpdate,   // <sym>@depth
    OrderUpdate,   // user stream: ORDER_TRADE_UPDATE
    AccountUpdate, // user stream: ACCOUNT_UPDATE
    ListenKeyExpired,
    SubscribeAck, // {"result":null,"id":1}
    Error,        // {"error":{"code":2,"msg":"..."},"id":1}
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty for control and account frames
};

// Peeks at the wrapper and the inner "e" without consuming the rest.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// Unwraps {"stream":...,"data":{...}} to the raw text of `data`. A frame that
// arrived on a single-stream socket has no wrapper and is returned unchanged.
[[nodiscard]] std::expected<std::string_view, WireError> unwrap(std::string_view frame);

// A refused SUBSCRIBE. Binance answers success with {"result":null,"id":N} and
// failure with an "error" object under the same id, so the two are one field
// apart — and a rejected subscription that reads as an acknowledgement is
// indistinguishable from a stream the venue simply never sends.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
    std::uint64_t id{0};
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

struct WireBookTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t update_id{0};
    std::uint64_t transaction_ms{0}; // T: when the book changed
    std::uint64_t event_ms{0};       // E: when binance sent it
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view data);

// One public print. Named for what it is rather than for the channel, because
// the same shape arrives on @trade and on @aggTrade and the adapter reads
// either; the account's own fill is WireTrade, further down.
struct WirePrint {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_id{0}; // aggregate id or trade id, whichever arrived
    std::uint64_t trade_ms{0};
    bool buyer_is_maker{false}; // m: true => the aggressor sold
};

[[nodiscard]] std::expected<WirePrint, WireError> parse_trade(std::string_view data);

// markPriceUpdate carries mark, index, funding rate and the next funding
// time together — one subscription covers what bybit splits across fields of
// its tickers stream.
struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ms{0};
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view data);

// GET /fapi/v1/premiumIndex, which carries the same facts as the markPriceUpdate
// stream under different names — lastFundingRate for r, nextFundingTime for T.
// It exists because that stream is acknowledged and then never sends a frame on
// this venue, so the rate has to be pulled or it does not arrive at all.
inline constexpr std::size_t k_max_premium_rows = 2048;

struct PremiumBuffer {
    std::array<WireMarkPrice, k_max_premium_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarkPrice>, WireError>
parse_premium_index(std::string_view body, PremiumBuffer& storage);

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

inline constexpr std::size_t k_max_levels = 1000;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// first_id/last_id are binance's [U, u]; prev_id is `pu`. The chaining rule
// on futures is prev_id == the previous event's last_id — NOT last_id + 1,
// which is the spot rule and would reject every valid futures delta.
struct WireDepthUpdate {
    std::string_view symbol;
    std::uint64_t first_id{0};
    std::uint64_t last_id{0};
    std::uint64_t prev_id{0};
    std::uint64_t event_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view data,
                                                                           LevelBuffer& storage);

// GET /fapi/v1/depth. `last_update_id` is the book state these levels
// describe, and the seed is usable only while a delta still straddles it.
struct WireDepthSnapshot {
    std::uint64_t last_update_id{0};
    std::uint64_t transaction_ms{0}; // T: when the book was that
    std::uint64_t event_ms{0};       // E: when binance answered
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthSnapshot, WireError>
parse_depth_snapshot(std::string_view body, LevelBuffer& storage);

// ------------------------------------------------------------ private ----

// ORDER_TRADE_UPDATE carries lifecycle AND fills in one frame, which is the
// load-bearing difference from bybit.
//
// Bybit splits them: its `execution` topic owns fills and its `order` topic
// owns lifecycle, and emitting from both doubles every position. Binance has
// one frame, so the same hazard appears from the other side — `x` (execution
// type) says WHAT HAPPENED and `X` (order status) says WHERE THE ORDER IS
// NOW. A TRADE execution also reports X = PARTIALLY_FILLED or FILLED, and
// treating that status as a second event double-counts the same fill.
//
// The rule, therefore: `x` alone decides which event we emit. `X` is carried
// for reconciliation and never turned into an event of its own.
struct WireOrderUpdate {
    std::string_view symbol;
    std::string_view client_order_id;   // c: our ExchangeClientOrderId
    std::string_view exec_type;         // x: NEW, CANCELED, EXPIRED, TRADE, ...
    std::string_view order_status;      // X: NEW, PARTIALLY_FILLED, FILLED, ...
    std::string_view side;              // S
    std::string_view order_type;        // o
    std::string_view time_in_force;     // f
    std::string_view order_qty;         // q
    std::string_view order_price;       // p
    std::string_view last_filled_qty;   // l
    std::string_view cum_filled_qty;    // z
    std::string_view last_filled_price; // L
    std::string_view commission;        // n — absent unless this frame is a fill
    std::string_view commission_asset;  // N
    std::uint64_t order_id{0};          // i
    std::uint64_t trade_id{0}; // aggregate id or trade id, whichever arrived          // t
    std::uint64_t trade_ms{0};          // T inside o
    std::uint64_t event_ms{0};          // E at the top level
    bool is_maker{false};               // m
    bool reduce_only{false};            // R
};

[[nodiscard]] std::expected<WireOrderUpdate, WireError> parse_order_update(std::string_view data);

// One position line out of ACCOUNT_UPDATE's a.P array.
struct WirePosition {
    std::string_view symbol;        // s
    std::string_view position_amt;  // pa: signed
    std::string_view entry_price;   // ep
    std::string_view position_side; // ps: BOTH, LONG, SHORT
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_account_update(std::string_view data, PositionBuffer& storage);

// ------------------------------------------------------------- rest ----

// Binance has no envelope: a success is the payload itself and a failure is
// {"code":-1121,"msg":"Invalid symbol."}. So an error is detected by the
// presence of a negative `code`, not by a status field — and `code` is a
// legitimate field name inside some success payloads, which is why this
// checks the sign rather than the key alone.
struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// Returns the error when the body is one; std::nullopt-shaped absence is
// expressed as a zero code, meaning "this body is a success payload".
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

struct WireInstrument {
    std::string_view symbol;
    std::string_view base_asset; // binance names both halves on every row
    std::string_view quote_asset;
    std::string_view contract_type; // PERPETUAL, CURRENT_QUARTER, ...
    std::string_view status;        // TRADING, SETTLING, PENDING_TRADING, ...
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_notional;
    std::uint64_t delivery_ms{0};
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_exchange_info(std::string_view body, InstrumentBuffer& storage);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// POST /fapi/v1/order and GET /fapi/v1/order share this shape; openOrders is
// an array of it.
struct WireOrder {
    std::string_view symbol;
    std::string_view client_order_id;
    std::string_view status;
    std::string_view side;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::uint64_t order_id{0};
    std::uint64_t update_ms{0};
};

[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view body);

inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::string_view commission;
    std::string_view side;
    std::uint64_t trade_id{0};
    std::uint64_t order_id{0};
    std::uint64_t time_ms{0};
    bool maker{false};
};

inline constexpr std::size_t k_max_trades = 1024;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_user_trades(std::string_view body, TradeBuffer& storage);

// One row of /fapi/v1/income, narrowed to settlements.
//
// `income` is SIGNED THE WAY THE ACCOUNT MOVED — negative was paid away — which
// is already the domain's convention, so nothing here flips it. `tranId` is
// binance's own id for the movement and is unique; it is kept for tracing, not
// for identity, because the socket states the same settlement without one.
struct WireIncome {
    std::string_view symbol;
    std::string_view income_type; // FUNDING_FEE | COMMISSION | REALIZED_PNL | ...
    std::string_view income;
    std::string_view asset;
    std::uint64_t tran_id{0};
    std::uint64_t time_ms{0};
};

inline constexpr std::size_t k_max_income_rows = 256;

struct IncomeBuffer {
    std::array<WireIncome, k_max_income_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireIncome>, WireError>
parse_income(std::string_view body, IncomeBuffer& storage);

struct WirePositionRisk {
    std::string_view symbol;
    std::string_view position_amt;
    std::string_view entry_price;
};

[[nodiscard]] std::expected<std::span<const WirePositionRisk>, WireError>
parse_position_risk(std::string_view body, std::span<WirePositionRisk> storage);

// One row of GET /fapi/v2/balance. Binance lists every asset it accepts as
// margin, not the ones the account holds, so most rows are zero across the
// board and the reply is mostly padding — which is a mapping decision, not a
// parsing one, and is made in map_balance.
struct WireBalance {
    std::string_view asset;             // asset
    std::string_view balance;           // wallet total
    std::string_view available_balance; // availableBalance: committable now
    std::string_view cross_unpnl;       // crossUnPnl
    std::uint64_t update_ms{0};         // 0 on an asset never held
};

// Binance answers ~30 rows today and adds one whenever it accepts another
// collateral asset. Sized far past that on purpose: refusing the whole body
// over rows that would have been dropped anyway reads as an account holding
// nothing.
inline constexpr std::size_t k_max_balances = 256;

struct BalanceBuffer {
    std::array<WireBalance, k_max_balances> balances{};
};

[[nodiscard]] std::expected<std::span<const WireBalance>, WireError>
parse_balances(std::string_view body, BalanceBuffer& storage);

// {"listenKey":"..."} — the user-stream key, valid 60 minutes and extended by
// a keepalive rather than reissued.
[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

} // namespace raikiri::venues::binance
