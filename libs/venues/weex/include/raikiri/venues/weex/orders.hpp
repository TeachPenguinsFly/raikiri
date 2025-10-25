#pragma once

// An order's life on the wire: what a REST answer says about it, and the fills
// it produced.
//
// NOTHING HERE HAS EVER SEEN A SUCCESSFUL BODY. There are no WEEX credentials
// on this machine, so every shape below is transcribed from the venue's own
// endpoint documentation and pinned against the examples printed there. That
// is enough to be wrong about, which is why each trap the docs contain is
// named rather than smoothed over.
//
// THERE IS NO `PARTIALLY_FILLED`. WEEX's whole order-status vocabulary is NEW,
// PENDING, UNTRIGGERED, UNACTIVATED, FILLED, CANCELED, CANCELING — an order
// half filled is still NEW, and the only thing that says so is `executedQty`
// against `origQty`. A reader that trusted the status word alone would report
// a working order as untouched while it filled.
//
// AN ORDER ID ARRIVES AS TWO DIFFERENT JSON TYPES. The place and cancel
// answers quote it as a STRING; the order, open-order and fill reads quote it
// as a NUMBER of eighteen digits. WEEX's own documentation shows what that
// costs: its open-order example prints 702345678901234600 for the order its
// place example called 702345678901234567, having lost the last three digits
// to a double on the way to the page. So the id is read as TEXT off the raw
// token, never as a float and never as one type only.

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

enum class WireOrderStatus : std::uint8_t {
    New, // working, WHETHER OR NOT it has partially filled
    Pending,
    Untriggered, // a conditional order that has not armed
    Unactivated,
    Filled,
    Canceled,
    Canceling, // the venue has accepted a cancel and not finished it
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_from(std::string_view text) noexcept;

// One row of GET /capi/v3/openOrders.
//
// WEEX answers the single-order read and the order history with the same
// object, and NEITHER IS BUILT: this process cannot look an order up by the
// only id it has (WEEX takes `orderId` there and no client id), and a parser
// with no caller is a facility nobody exercises and everybody trusts. The row
// also carries positionSide, orderType, avgPrice, cumQuote and stopPrice, and
// none of them is read here for the same reason — an OrderStatusReport has
// nowhere to put them.
struct WireOrder {
    std::string_view symbol;
    std::string_view order_id;        // orderId, read as text; see the note above
    std::string_view client_order_id; // clientOrderId: our ULID comes back here
    std::string_view side;            // BUY | SELL
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty; // CUMULATIVE, so leaves is a subtraction
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t update_ms{0};
};

// A reconciliation page is capped at 100 rows by the venue's own `limit`.
inline constexpr std::size_t k_max_orders = 128;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// A BARE ARRAY, with no envelope around it.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// The answer to POST /capi/v3/order and DELETE /capi/v3/order.
//
// A 200 IS NOT AN ACCEPTANCE. WEEX reports a refused order inside a successful
// HTTP response by setting `success` false and filling `errorCode` — a string
// name like "ORDER_NOT_FOUND", not one of the numeric codes — so a caller that
// stopped at the status line would record a rejection as a live order.
struct WireOrderAck {
    std::string_view order_id;
    std::string_view client_order_id; // clientOrderId on a place, origClientOrderId on a cancel
    std::string_view error_code;
    std::string_view error_message;
    bool success{false};
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body);

// The batch cancel answers the same object, once per order, in a BARE ARRAY —
// and a batch is per-order: one entry saying `success` false beside nine saying
// true is the normal shape, not an error.
inline constexpr std::size_t k_max_acks = 16;

struct OrderAckBuffer {
    std::array<WireOrderAck, k_max_acks> acks{};
};

[[nodiscard]] std::expected<std::span<const WireOrderAck>, WireError>
parse_order_acks(std::string_view body, OrderAckBuffer& storage);

// The one `errorCode` WEEX prints for an order it no longer has. A STRING,
// unlike the numeric codes a whole-request failure carries, and the only value
// of it the venue has ever put on a page — which is why nothing here pretends
// to a list.
[[nodiscard]] bool is_benign_cancel_error(std::string_view error_code) noexcept;

// One row of GET /capi/v3/userTrades.
//
// NO CLIENT ORDER ID RIDES A FILL. WEEX names only its own `orderId` here, so
// a fill is attributed through that and the report carries no ULID rather than
// a guess at one.
struct WireFill {
    std::string_view symbol;
    std::string_view fill_id;  // id
    std::string_view order_id; // orderId
    std::string_view side;     // BUY | SELL
    std::string_view price;
    std::string_view qty;        // base asset
    std::string_view commission; // POSITIVE IS CHARGED
    bool maker{false};
    std::uint64_t fill_ms{0};
};

// A page of fills is capped at 100 by the venue's own `limit`.
inline constexpr std::size_t k_max_fills = 128;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

} // namespace raikiri::venues::weex
