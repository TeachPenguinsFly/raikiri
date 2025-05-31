#pragma once

// Bitget v2 wire types and parsers, USDT-M futures.
//
// The error code is a STRING over REST and a JSON NUMBER over the websocket,
// verified live: {"code":"40034",...} against {"event":"error","code":30001}.
// Different types AND different code spaces, so one path per transport. The
// REST envelope also succeeds on "00000", not "0" — five characters that any
// integer conversion turns into the zero a missing field would produce, which
// is why the envelope keeps the text.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

using WireError = wire::Error;

// Resolved from arg.channel once, so dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Ticker, // bid/ask, mark, index and funding on one frame
    Trade,
    Books,  // books5/books15/books share one shape
    Orders, // private: lifecycle AND fills on one channel
    LoginAck, // the private handshake's answer, distinct from a subscribe ack
    SubscribeAck,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view inst_id; // empty on control frames
};

// The keepalive answer is the bare text "pong", so it lands here as NotJson
// and is ignored — the same path as any frame Bitget adds later.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription: {"event":"error","code":30001,"msg":"...",...}.
// Note the unquoted code; the REST envelope's is quoted.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// One `ticker` element. index_price is carried but unmapped: nothing above
// consumes an index yet, and dropping it would hide that Bitget sends it.
struct WireTicker {
    std::string_view inst_id;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::string_view last_price;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `side` is already the AGGRESSOR's side — a print at the bid reads "sell",
// confirmed against a live book — so nothing is inverted here or in the
// mapper. Binance's `m` says the opposite and needs a flip; reading the two
// the same way is how a signed flow series comes out backwards.
struct WireTrade {
    std::string_view inst_id; // from the frame's arg: the element carries none
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// `books` snapshots 500 levels a side; the ceiling leaves room without
// letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 1024;

// Bitget caps a cancel batch at 50, so a failure list cannot exceed it.
inline constexpr std::size_t k_max_cancel_rows = 50;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the seq chain: an update applies when its prev_seq_id equals
// the previous message's seq_id. books15 restates the whole top of book and
// pins prev_seq_id to 0, so only the incremental `books` channel has a chain
// worth checking — 0 for "no predecessor", where okx uses -1.
struct WireBooks {
    std::string_view inst_id;
    bool snapshot{false}; // action: "snapshot" vs "update"
    std::int64_t seq_id{0};
    std::int64_t prev_seq_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBooks, WireError> parse_books(std::string_view frame,
                                                              LevelBuffer& storage);

// ----------------------------------------------------------- private ----

// Bitget's lifecycle vocabulary. SPOT spells the terminal cancel with two Ls
// and MIX with one, and both are live: reading only one spelling leaves half
// the cancels looking like orders that merely stopped updating.
enum class WireOrderStatus : std::uint8_t {
    Live,
    PartiallyFilled,
    Filled,
    Canceled,
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_from(std::string_view text) noexcept;

// One `orders` element, and the same reader serves the REST pending lists —
// which name the instrument `symbol` where the socket names it `instId`.
//
// The channel carries the fill inline: a push caused by a trade adds
// trade_id, base_volume and fill_price to the lifecycle fields, which is why
// this adapter never subscribes the separate `fill` channel. Subscribing both
// would double-count every trade, and the spot `fill` element carries no
// clientOid, so half of them would arrive unattributable.
struct WireOrderUpdate {
    std::string_view inst_id;
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view side;       // buy, sell
    std::string_view order_type; // limit, market
    std::string_view force;      // gtc, post_only, fok, ioc
    std::string_view price;
    // Order quantity. Spot states it in the QUOTE coin on a market buy and in
    // the base coin otherwise; `new_size` is spot's base-denominated field.
    std::string_view size;
    std::string_view new_size;
    std::string_view acc_base_volume; // cumulative filled, base units
    std::string_view base_volume;     // this fill alone
    std::string_view fill_price;
    std::string_view fill_fee;
    std::string_view trade_id;
    std::string_view trade_scope; // "T"/"M" here; the `fill` channel spells it out
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t fill_ms{0};
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_updates = 128;

struct OrderUpdateBuffer {
    std::array<WireOrderUpdate, k_max_updates> updates{};
};

// The private socket's `orders` push.
[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError>
parse_order_updates(std::string_view frame, OrderUpdateBuffer& storage);

// GET .../spot/trade/unfilled-orders — `data` IS the array.
// GET .../spot/trade/orderInfo — likewise an array, even for one order.
[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError>
parse_spot_orders(std::string_view body, OrderUpdateBuffer& storage);

// GET .../mix/order/orders-pending — the array hides under data.entrustedList.
[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError>
parse_mix_orders(std::string_view body, OrderUpdateBuffer& storage);

// A fill row from either fills endpoint. Neither carries a clientOid, so the
// venue order id is the only join back to an order.
struct WireFill {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view trade_id;
    std::string_view side;
    std::string_view price;
    std::string_view size;
    std::string_view fee;         // feeDetail totalFee; negative on Bitget
    std::string_view trade_scope; // taker, maker
    std::uint64_t ts_ms{0};
};

struct FillBuffer {
    std::array<WireFill, k_max_updates> fills{};
};

// GET .../spot/trade/fills — `data` is the array and `feeDetail` an object.
[[nodiscard]] std::expected<std::span<const WireFill>, WireError>
parse_spot_fills(std::string_view body, FillBuffer& storage);

// GET .../mix/order/fills — data.fillList, and `feeDetail` is an ARRAY.
[[nodiscard]] std::expected<std::span<const WireFill>, WireError>
parse_mix_fills(std::string_view body, FillBuffer& storage);

// `holdSide` names the direction; there is no signed size anywhere.
struct WirePosition {
    std::string_view symbol; // `instId` on the socket, `symbol` over REST
    std::string_view hold_side;
    std::string_view total;
    std::string_view open_price_avg;
    std::uint64_t update_ms{0};
};

struct PositionBuffer {
    std::array<WirePosition, k_max_updates> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

// The `data` of place-order and cancel-order. order_id is empty when Bitget
// declines to name it — a documented outcome when a one-way reduce-only
// order displaces earlier ones — and clientOid is then the only handle.
struct WireOrderAck {
    std::string_view order_id;
    std::string_view client_oid;
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body);

// Batch cancels answer 00000 at the envelope while refusing individual
// orders inside failureList, so a caller that reads only the envelope
// believes it cancelled orders that are still working.
struct WireBatchFailure {
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view code;
    std::string_view msg;
};

struct BatchFailureBuffer {
    std::array<WireBatchFailure, k_max_cancel_rows> failures{};
};

[[nodiscard]] std::expected<std::span<const WireBatchFailure>, WireError>
parse_batch_failures(std::string_view body, BatchFailureBuffer& storage);

// ------------------------------------------------------------- rest ----

inline constexpr std::string_view k_ok = "00000";

// Text, not an integer: "00000" and a missing code convert to the same zero.
struct RestEnvelope {
    std::string_view code;
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// One /api/v2/mix/market/contracts row. Neither price field alone is the
// tick — see map.cpp.
struct WireContract {
    std::string_view symbol;
    std::string_view base_coin;
    std::string_view quote_coin;
    std::string_view symbol_type;   // perpetual, delivery
    std::string_view symbol_status; // normal, maintain, limit_open, off, ...
    std::string_view price_place;
    std::string_view price_end_step;
    std::string_view size_multiplier; // the quantity step
    std::string_view min_trade_num;   // a lot floor, not a notional one
    std::string_view min_trade_usdt;
    std::string_view fund_interval; // hours: 1, 4 and 8 are all live
};

// 745 USDT-FUTURES rows on 2026-08-11 and the list only grows.
inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// One /api/v2/spot/public/symbols row. Spot states precision as DECIMAL
// PLACES and ships no priceEndStep at all, so the mix row's two-factor tick
// has no analogue here — reading the two catalogues through one struct is how
// a spot tick silently gains a factor it never had.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view base_coin;
    std::string_view quote_coin;
    std::string_view status; // online, halt, offline
    std::string_view price_precision;
    std::string_view quantity_precision;
    std::string_view min_trade_amount;
    std::string_view min_trade_usdt;
};

// 1280 spot rows on 2026-08-21 against 745 mix rows, and both only grow.
inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// /api/v2/mix/market/current-fund-rate. Its field names match nothing else
// Bitget publishes: `fundingRateInterval` in hours, and `nextUpdate` for the
// instant the ticker channel calls `nextFundingTime`.
struct WireFundingRate {
    std::string_view symbol;
    std::string_view funding_rate;
    std::string_view interval_hours;
    std::string_view min_rate;
    std::string_view max_rate;
    std::uint64_t next_update_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError>
parse_current_fund_rate(std::string_view body);

// /api/v2/mix/market/orderbook. Unlike every other endpoint here its `data`
// is an OBJECT, not a single-element array, so it needs its own reader.
[[nodiscard]] std::expected<WireBooks, WireError> parse_rest_orderbook(std::string_view body,
                                                                       LevelBuffer& storage);

} // namespace raikiri::venues::bitget
