#pragma once

// An order's life on the wire, and the executions it produced.
//
// One reader serves REST and the websocket for both tables here, because the
// ROW is the same object: POST /order answers an `order` row, the private
// `order` table streams the same row, and only the wrapper differs — a bare
// JSON array over REST, a `data` array over the socket.
//
// The partial-column rule of the public tables applies here too and matters
// more: BitMEX keys `order` on **orderID alone**, so an `update` can arrive
// carrying an id, an ordStatus and nothing else — no symbol, no side, no size.
// Every field but the id is optional, and a caller that needs the instrument
// must have remembered it from the row that created the order.

#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitmex {

struct WireOrder {
    std::string_view order_id;
    std::string_view cl_ord_id;
    std::string_view symbol;
    std::string_view side;       // Buy | Sell
    std::string_view ord_status; // New | PartiallyFilled | Filled | Canceled | Rejected
    std::string_view ord_type;   // Limit | Market | Stop | ...
    std::string_view time_in_force;
    std::string_view exec_inst;
    std::string_view ord_rej_reason;
    std::string_view text;
    // A bulk DELETE answers HTTP 200 with the per-row verdict HERE: an order it
    // could not cancel comes back carrying `error` beside every other column.
    // Reading the status line alone reports that cancel as successful.
    std::string_view error;
    std::string_view price;
    std::string_view order_qty;
    std::string_view leaves_qty;
    std::string_view cum_qty;
    std::string_view transact_time;
    std::string_view timestamp;
};

inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view payload, OrderBuffer& storage);

// POST and PUT /order answer ONE row; DELETE and GET answer an array, and the
// websocket wraps rows in `data`. Two entry points rather than a shape sniff:
// the caller knows which it asked for, and guessing is how an empty array comes
// to read as a refusal.
[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view body);

// One row of `execution`. `execType` names WHY the row exists and only "Trade"
// is a fill: "Funding" and "Settlement" rows carry a price and a quantity too,
// and booking them as trades would invent a position that was never traded.
//
// `commission` is the fee RATE for this fill, signed — a maker rebate is
// negative. The AMOUNT beside it, `execComm`, is denominated in the settlement
// currency's minor units (satoshis for XBt) and the row does not say what that
// currency's scale is, so the rate is what travels.
struct WireExecution {
    std::string_view exec_id;
    std::string_view order_id;
    std::string_view cl_ord_id;
    std::string_view symbol;
    std::string_view side;
    std::string_view exec_type;
    std::string_view ord_status;
    std::string_view last_qty;
    std::string_view last_px;
    std::string_view leaves_qty;
    std::string_view cum_qty;
    std::string_view last_liquidity_ind; // Maker | RemovedLiquidity | ...
    std::string_view commission;
    std::string_view transact_time;
};

inline constexpr std::size_t k_max_executions = 512;

struct ExecutionBuffer {
    std::array<WireExecution, k_max_executions> executions{};
};

[[nodiscard]] std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view payload, ExecutionBuffer& storage);

} // namespace raikiri::venues::bitmex
