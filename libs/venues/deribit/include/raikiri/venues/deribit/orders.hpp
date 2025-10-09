#pragma once

// An order's life on the wire, and the executions it produced. Deribit
// answers the identical row from seven endpoints and from the user.changes
// channel, so one reader serves all of them and a schema drift shows up in
// every one at once.

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

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
    std::string_view fee; // in fee_currency, which is the base coin
    std::string_view fee_currency;
    std::string_view state; // the ORDER's state after this trade
    bool sell{false};
    bool maker{false};            // liquidity: "M" maker, "T" taker
    bool liquidity_stated{false}; // absent on some historical rows
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

} // namespace raikiri::venues::deribit
