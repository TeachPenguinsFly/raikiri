#pragma once

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

// amount and filled_amount are in the unit the instrument's lot_size divides
struct WireOrder {
    std::string_view order_id;
    std::string_view label; // the ULID this adapter sent as the client id
    std::string_view instrument;
    std::string_view state; // open, filled, rejected, cancelled, (un)triggered
    // ioc and fok remainders also report cancelled; only tif tells them apart
    std::string_view tif;
    // empty when deribit sends market_price for an open trigger market order
    std::string_view price;
    std::string_view amount;
    std::string_view filled_amount;
    std::string_view average_price;
    bool sell{false};
    std::uint64_t ts_ms{0}; // last_update_timestamp
};

// larger frames are refused whole
inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// direction is the user's own side
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

// the venue caps count at 1000
inline constexpr std::size_t k_max_user_trades = 256;

struct UserTradeBuffer {
    std::array<WireUserTrade, k_max_user_trades> trades{};
};

// buy and edit return the order and the trades it caused together
struct WireOrderAck {
    WireOrder order{};
    std::span<const WireUserTrade> trades;
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body, UserTradeBuffer& storage);

// result is an array on list reads and an object on private/cancel
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                                OrderBuffer& storage);

// orders and the trades that moved them arrive in one frame
struct WireChanges {
    std::span<const WireOrder> orders;
    std::span<const WireUserTrade> trades;
};

[[nodiscard]] std::expected<WireChanges, WireError>
parse_user_changes(std::string_view frame, OrderBuffer& orders, UserTradeBuffer& trades);

// rest nests trades under result.trades; the socket puts them in params.data
[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError> parse_user_trades(std::string_view body,
                                                                                         UserTradeBuffer& storage);

} // namespace raikiri::venues::deribit
