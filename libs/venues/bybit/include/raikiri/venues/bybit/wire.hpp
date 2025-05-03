#pragma once

// Bybit v5 wire types and parsers.
//
// Parsing is simdjson On-Demand over the read buffer, so a frame is walked
// once with no intermediate document. Every parse function is a fuzz target:
// venue JSON is untrusted input arriving at line rate, and the failure mode
// we refuse is a malformed frame taking down a gateway that holds live
// orders. Missing or unexpected fields yield an error, never a partial
// struct with a plausible-looking zero in it.

#include <raikiri/common/time.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::bybit {

using WireError = wire::Error;

// Which stream a public frame belongs to. Resolved from "topic" once, so the
// dispatch that follows is an integer compare rather than a string compare
// per message.
enum class TopicKind : std::uint8_t {
    Orderbook,   // orderbook.{depth}.{symbol}
    PublicTrade, // publicTrade.{symbol}
    Tickers,     // tickers.{symbol}
    Order,       // private
    Execution,   // private
    Position,    // private
    Wallet,      // private
    Pong,        // {"op":"pong"} / {"ret_msg":"pong"}
    OpResponse,  // subscribe/auth acknowledgement
    Unknown,
};

struct TopicInfo {
    TopicKind kind{TopicKind::Unknown};
    std::string_view symbol; // empty for private and control frames
};

// Peeks at "topic"/"op" without consuming the rest of the frame.
[[nodiscard]] std::expected<TopicInfo, WireError> parse_topic(std::string_view frame);

// ------------------------------------------------------------ public ----

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0" means delete this level
};

// orderbook.N.SYMBOL. `is_snapshot` distinguishes a full book from a delta;
// `update_id` and `seq` are bybit's ordering guarantees — a gap in update_id
// means the book must be resynchronised, which is why they are surfaced
// rather than swallowed.
struct WireOrderbook {
    std::string_view symbol;
    bool is_snapshot{false};
    std::uint64_t update_id{0};
    std::uint64_t seq{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

inline constexpr std::size_t k_max_levels = 512;

// Storage the caller owns and reuses across frames — parsing must not
// allocate on the market-data path.
struct OrderbookBuffer {
    WireLevel bids[k_max_levels];
    WireLevel asks[k_max_levels];
};

[[nodiscard]] std::expected<WireOrderbook, WireError> parse_orderbook(std::string_view frame,
                                                                      OrderbookBuffer& storage);

struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    bool buyer_aggressor{false}; // "S":"Buy" means the taker bought
    std::uint64_t ts_ms{0};
    std::string_view trade_id;
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    WireTrade trades[k_max_trades];
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// tickers.SYMBOL on linear perps pushes a full snapshot once and then
// DELTAS containing only changed fields — so every field here is optional
// and an absent one means "unchanged", not "zero". Getting this wrong
// silently zeroes a funding rate, which is why the flags exist.
struct WireTicker {
    std::string_view symbol;
    bool is_snapshot{false};
    std::string_view mark_price; // empty when absent
    std::string_view index_price;
    std::string_view funding_rate;
    std::string_view next_funding_ms;
    std::string_view bid1_price;
    std::string_view bid1_size;
    std::string_view ask1_price;
    std::string_view ask1_size;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// ----------------------------------------------------------- private ----

// Bybit order lifecycle values. PartiallyFilledCanceled is the one that
// surprises people: it is terminal, not a partial fill.
enum class WireOrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Untriggered,
    Triggered,
    Deactivated,
    Rejected,
    Cancelled,
    PartiallyFilledCanceled,
    Filled,
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_from(std::string_view text) noexcept;

struct WireOrderUpdate {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view order_link_id; // our ULID comes back here
    std::string_view side;
    std::string_view order_type;
    std::string_view price;
    std::string_view qty;
    std::string_view leaves_qty;
    std::string_view cum_exec_qty;
    std::string_view avg_price;
    std::string_view reject_reason;
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t updated_ms{0};
};

inline constexpr std::size_t k_max_updates = 64;

struct OrderUpdateBuffer {
    WireOrderUpdate updates[k_max_updates];
};

[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError>
parse_order_updates(std::string_view frame, OrderUpdateBuffer& storage);

struct WireExecution {
    std::string_view symbol;
    std::string_view exec_id;
    std::string_view order_id;
    std::string_view order_link_id;
    std::string_view side;
    std::string_view exec_price;
    std::string_view exec_qty;
    std::string_view exec_fee;
    std::string_view leaves_qty;
    bool is_maker{false};
    std::uint64_t exec_ms{0};
};

struct ExecutionBuffer {
    WireExecution executions[k_max_updates];
};

[[nodiscard]] std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view frame, ExecutionBuffer& storage);

struct WirePosition {
    std::string_view symbol;
    std::string_view side; // "Buy", "Sell", or "" when flat
    std::string_view size;
    std::string_view avg_price;
    std::uint64_t updated_ms{0};
};

struct PositionBuffer {
    WirePosition positions[k_max_updates];
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view frame, PositionBuffer& storage);

// -------------------------------------------------------------- REST ----

// Every v5 REST response wraps its payload in this envelope. retCode == 0
// is success; anything else carries the venue's reason, which the adapter
// classifies rather than logging and forgetting.
struct WireRestEnvelope {
    std::int64_t ret_code{0};
    std::string_view ret_msg;
    std::string_view result; // raw JSON of "result", parsed by the caller
    std::uint64_t time_ms{0};
};

[[nodiscard]] std::expected<WireRestEnvelope, WireError> parse_envelope(std::string_view body);

// result of /v5/order/create and /v5/order/cancel
struct WireOrderAck {
    std::string_view order_id;
    std::string_view order_link_id;
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view result);

// One entry of /v5/order/realtime (open orders) — the reconciliation pull.
using WireOpenOrder = WireOrderUpdate;

[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view result, OrderUpdateBuffer& storage);

// One entry of /v5/market/instruments-info.
struct WireInstrument {
    std::string_view symbol;
    std::string_view base_coin;
    std::string_view quote_coin;
    std::string_view tick_size;
    std::string_view qty_step;
    std::string_view min_order_qty;
    std::string_view min_notional;
    std::string_view status; // "Trading" for live instruments
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    WireInstrument instruments[k_max_instruments];
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view result, InstrumentBuffer& storage);

} // namespace raikiri::venues::bybit
