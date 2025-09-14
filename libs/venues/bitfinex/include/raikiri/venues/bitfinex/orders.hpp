#pragma once

// An order's life on the wire, the fills it produced, and the answer to every
// write.
//
// THE SAME POSITIONAL RULE AS THE PUBLIC SIDE, AND IT COSTS MORE HERE: SIZES
// ARE SIGNED AND THE SIGN IS NOT THE SIZE. A negative AMOUNT on an order is a
// SELL and on an execution a sale — never a negative quantity. OrderEvent
// carries a Side beside a positive QtyLots, and carrying the sign into the
// quantity instead inverts the fill by twice its size with nothing downstream
// able to notice.
//
// The private frames arrive on CHANNEL 0 with a tag: `os`/`on`/`ou`/`oc` for
// orders, `te`/`tu` for executions, and `n` for the answer to a write. The
// same arrays come back from REST.

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// order: [ID,GID,CID,SYMBOL,MTS_CREATE,MTS_UPDATE,AMOUNT,AMOUNT_ORIG,TYPE,
//         TYPE_PREV,MTS_TIF,_,FLAGS,STATUS,_,_,PRICE,PRICE_AVG,...] — 32 long.
//
// The arity below is what is READ, not what arrives: the tail past PRICE_AVG
// is trailing-stop, OCO and routing fields this adapter has no use for, and
// requiring all 32 would refuse a frame over a field nobody reads.
//
// AMOUNT is what REMAINS and AMOUNT_ORIG what was asked for, both signed. The
// SIDE MUST COME FROM AMOUNT_ORIG: a fully executed order has AMOUNT 0, and
// zero has no sign, so a reader taking the side from AMOUNT reports every
// completed sale as a buy.
inline constexpr std::size_t k_order_arity = 18;
inline constexpr std::size_t k_order_id = 0;
inline constexpr std::size_t k_order_gid = 1;
inline constexpr std::size_t k_order_cid = 2;
inline constexpr std::size_t k_order_symbol = 3;
inline constexpr std::size_t k_order_mts_create = 4;
inline constexpr std::size_t k_order_mts_update = 5;
inline constexpr std::size_t k_order_amount = 6;
inline constexpr std::size_t k_order_amount_orig = 7;
inline constexpr std::size_t k_order_type = 8;
inline constexpr std::size_t k_order_flags = 12;
inline constexpr std::size_t k_order_status = 13;
inline constexpr std::size_t k_order_price = 16;
inline constexpr std::size_t k_order_price_avg = 17;

// STATUS is PROSE WITH THE FILL GLUED ON: "EXECUTED @ 63947.0(0.1)",
// "PARTIALLY FILLED @ ...", "INSUFFICIENT MARGIN was: ...". Comparing it for
// equality matches only "ACTIVE" and "CANCELED" and silently drops every fill,
// so it is matched by PREFIX and resolved to this enum once per record.
enum class WireOrderStatus : std::uint8_t {
    Active,
    PartiallyFilled,
    Executed,
    Canceled, // includes POSTONLY CANCELED
    Rejected, // INSUFFICIENT MARGIN / INSUFFICIENT BALANCE / RSN_*
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status(std::string_view text) noexcept;

// Order flags, the bitfield Bitfinex takes instead of separate booleans.
inline constexpr std::int64_t k_flag_hidden = 64;
inline constexpr std::int64_t k_flag_close = 512;
inline constexpr std::int64_t k_flag_reduce_only = 1024;
inline constexpr std::int64_t k_flag_post_only = 4096;
inline constexpr std::int64_t k_flag_oco = 16384;

struct WireOrder {
    std::string_view id;
    std::string_view symbol;
    std::string_view amount;      // SIGNED, and what REMAINS
    std::string_view amount_orig; // SIGNED, and the only reliable side
    std::string_view type;        // "EXCHANGE LIMIT" on spot, "LIMIT" on margin
    std::string_view price;
    std::string_view price_avg;
    std::string_view status_text; // kept verbatim: it carries the fill detail
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::int64_t cid{0};
    std::int64_t flags{0};
    std::uint64_t mts_create{0};
    std::uint64_t mts_update{0};
};

// A page of orders.
inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// One order: the `on`/`ou`/`oc` push payload, or a REST single-order row.
[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view frame);

// The `os` snapshot, and every REST order read.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// execution: [ID,SYMBOL,MTS_CREATE,ORDER_ID,EXEC_AMOUNT,EXEC_PRICE,ORDER_TYPE,
//             ORDER_PRICE,MAKER,FEE,FEE_CURRENCY]
//
// EXEC_AMOUNT IS SIGNED AND THE SIGN IS OUR SIDE — not the aggressor's, the
// way it is on the public trades channel. MAKER is 1 or -1, not a boolean.
//
// `te` and `tu` are the SAME fill twice: `te` arrives immediately with FEE
// null, `tu` restates it once the fee is known. A consumer that emits both
// double-counts every fill, so only ONE of them may reach the sink.
//
// The docs place CID at index 11; Bitfinex's own model stops at 10 and no live
// capture exists to settle it, so the arity stops where the SDK does and a
// fill is attributed through the order id instead.
inline constexpr std::size_t k_exec_arity = 11;
inline constexpr std::size_t k_exec_id = 0;
inline constexpr std::size_t k_exec_symbol = 1;
inline constexpr std::size_t k_exec_mts = 2;
inline constexpr std::size_t k_exec_order_id = 3;
inline constexpr std::size_t k_exec_amount = 4;
inline constexpr std::size_t k_exec_price = 5;
inline constexpr std::size_t k_exec_order_type = 6;
inline constexpr std::size_t k_exec_order_price = 7;
inline constexpr std::size_t k_exec_maker = 8;
inline constexpr std::size_t k_exec_fee = 9;
inline constexpr std::size_t k_exec_fee_currency = 10;

struct WireExecution {
    std::string_view id;
    std::string_view symbol;
    std::string_view order_id;
    std::string_view amount; // SIGNED; the sign is OUR side
    std::string_view price;
    std::string_view fee;          // empty on `te`, where it is not yet known
    std::string_view fee_currency; // likewise
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t mts{0};
};

// Its own, though it happens to match the order buffer's, and deliberately not
// shared with it or with the position buffer: one ceiling for all three makes
// a change to any of them silently resize the other two.
inline constexpr std::size_t k_max_executions = 512;

struct ExecutionBuffer {
    std::array<WireExecution, k_max_executions> executions{};
};

[[nodiscard]] std::expected<WireExecution, WireError> parse_execution(std::string_view frame);

[[nodiscard]] std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view body, ExecutionBuffer& storage);

// notification: [MTS,TYPE,MESSAGE_ID,_,PAYLOAD,CODE,STATUS,TEXT]
//
// This is the answer to EVERY write, over REST and over the socket alike, and
// STATUS is the verdict: "SUCCESS" or "ERROR". Bitfinex answers HTTP 200 to a
// rejected order, so an adapter that reads the status code and not this field
// reports every refusal as an acceptance.
inline constexpr std::size_t k_notify_arity = 8;
inline constexpr std::size_t k_notify_mts = 0;
inline constexpr std::size_t k_notify_type = 1;
inline constexpr std::size_t k_notify_payload = 4;
inline constexpr std::size_t k_notify_code = 5;
inline constexpr std::size_t k_notify_status = 6;
inline constexpr std::size_t k_notify_text = 7;

struct WireNotification {
    std::string_view type;   // "on-req" | "ou-req" | "oc-req" | "oc_multi-req"
    std::string_view status; // "SUCCESS" | "ERROR" | "FAILURE"
    std::string_view text;
    std::int64_t code{0};
    std::uint64_t mts{0};
    bool has_order{false};
    WireOrder order{}; // PAYLOAD, when the write named exactly one order
};

[[nodiscard]] bool notification_ok(const WireNotification& notification) noexcept;

[[nodiscard]] std::expected<WireNotification, WireError> parse_notification(std::string_view body);

} // namespace raikiri::venues::bitfinex
