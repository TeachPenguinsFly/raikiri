#pragma once

// An order's life on the wire: what the channel says happened to it, what the
// REST lists say about it, the fills it produced, and what a place or cancel
// answered.
//
// One row reader serves the socket and both REST lists on purpose: Bitget
// ships the same order under three field spellings (instId vs symbol, status
// vs state, accBaseVolume vs baseVolume) and three envelopes (`data`,
// `data.entrustedList`, `data.fillList`). Three readers would drift apart on
// the fields they agree about.

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

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

// Bitget caps a cancel batch at 50, so a failure list cannot exceed it.
inline constexpr std::size_t k_max_cancel_rows = 50;

struct BatchFailureBuffer {
    std::array<WireBatchFailure, k_max_cancel_rows> failures{};
};

[[nodiscard]] std::expected<std::span<const WireBatchFailure>, WireError>
parse_batch_failures(std::string_view body, BatchFailureBuffer& storage);

} // namespace raikiri::venues::bitget
