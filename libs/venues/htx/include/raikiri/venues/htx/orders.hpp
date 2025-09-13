#pragma once

// An order's life on the wire, across BOTH trees: the cross-margin swap
// endpoints on api.hbdm.com and the spot endpoints on api.huobi.pro. The two
// spell the same facts differently and neither is a superset of the other, so
// every reader here takes the MarketType that says which is being read.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

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

// The swap sweeps page at 50 and spot's list endpoints cap at 500.
inline constexpr std::size_t k_max_orders = 512;

// One websocket order frame reports at most a handful of trades; the ceiling
// exists so a hostile frame cannot make us walk past the buffer.
inline constexpr std::size_t k_max_fills_per_frame = 64;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
    std::array<WireFill, k_max_fills_per_frame> fills{};
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

// REST bodies -----------------------------------------------------------

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market);

} // namespace raikiri::venues::htx
