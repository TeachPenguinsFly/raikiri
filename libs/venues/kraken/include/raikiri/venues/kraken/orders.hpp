#pragma once

// An order's life on the wire: the verdict a REST write answers with, the
// working set, the open_orders feed's two shapes, and the fills both surfaces
// report.

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// The one-order verdict. sendorder, cancelorder and editorder answer the same
// object under three different keys, so the caller names which it asked for
// rather than the parser guessing from what it finds.
enum class StatusKind : std::uint8_t { Send, Cancel, Edit };

// `status` is Kraken's own vocabulary ("placed", "cancelled", "invalidSize",
// "postWouldExecute", ...) and is the ONLY verdict: the envelope reports
// result "success" for a refused order as readily as for an accepted one.
struct WireOrderStatus {
    std::string_view status;
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::uint64_t received_ms{0};
};

[[nodiscard]] std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body,
                                                                           StatusKind kind);

// POST /batchorder answers one status per element, in request order, under
// `batchStatus`. A batch whose elements carry no order_tag can be matched only
// positionally, which is why the count matters as much as the contents.
inline constexpr std::size_t k_max_batch = 50;

struct BatchStatusBuffer {
    std::array<WireOrderStatus, k_max_batch> statuses{};
};

[[nodiscard]] std::expected<std::span<const WireOrderStatus>, WireError>
parse_batch_status(std::string_view body, BatchStatusBuffer& storage);

// GET /openorders. `unfilledSize` is what is LEFT and `filledSize` what has
// traded; neither is the original size, which the row never states.
struct WireOpenOrder {
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::string_view symbol;
    std::string_view status;     // untouched | partiallyFilled
    std::string_view order_type; // lmt | stop | take_profit
    std::string_view limit_price;
    std::string_view filled_size;
    std::string_view unfilled_size;
    std::uint64_t last_update_ms{0};
    bool buy{false};
    bool reduce_only{false};
};

inline constexpr std::size_t k_max_open_orders = 1024;

struct OpenOrderBuffer {
    std::array<WireOpenOrder, k_max_open_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view body, OpenOrderBuffer& storage);

// GET /fills. `fill_time` is kept as TEXT as well as milliseconds: the
// lastFillTime cursor that pages backwards through this endpoint is the very
// string the venue sent, so echoing it avoids re-rendering an instant whose
// fractional width varies frame to frame.
struct WireFill {
    std::string_view fill_id;
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    std::string_view fill_type; // maker | taker | liquidation | ...
    std::string_view fill_time;
    std::uint64_t fill_ms{0};
    bool buy{false};
};

// The endpoint answers at most 100 rows per page; the ceiling is slack.
inline constexpr std::size_t k_max_fills = 256;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

// The open_orders order object.
//
// `qty` is what REMAINS, not what was asked for — VERIFIED against the field's
// own companion `filled`, which is cumulative, and against the venue's
// reference mappings. Reading qty as the original size turns a half-filled
// order into a smaller order that never filled.
struct WireWsOrder {
    std::string_view instrument;
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::string_view type; // limit | stop | take_profit | trailing_stop
    std::string_view remaining_qty;
    std::string_view filled_qty;
    std::string_view limit_price;
    std::string_view stop_price;
    std::uint64_t time_ms{0};
    std::uint64_t last_update_ms{0};
    bool buy{false};
    bool reduce_only{false};
};

// A delta is one of two shapes. A removal carries `order_id` at the root and
// no order object at all, which is why an adapter must remember what it sent:
// nothing in that frame names the instrument.
struct WireOrderDelta {
    std::string_view order_id;
    std::string_view reason;
    bool is_cancel{false};
    bool has_order{false};
    WireWsOrder order{};
};

[[nodiscard]] std::expected<WireOrderDelta, WireError> parse_order_delta(std::string_view frame);

struct OrderSnapshotBuffer {
    std::array<WireWsOrder, k_max_open_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireWsOrder>, WireError>
parse_order_snapshot(std::string_view frame, OrderSnapshotBuffer& storage);

// The fills feed. `fee_paid` is signed and denominated in `fee_currency`,
// which is the BASE asset on the inverse (PI_) families and the quote asset on
// the linear (PF_) ones — the same split ContractKind already records.
struct WireWsFill {
    std::string_view instrument;
    std::string_view order_id;
    std::string_view fill_id;
    std::string_view cli_ord_id;
    std::string_view price;
    std::string_view qty;
    std::string_view remaining_order_qty;
    std::string_view fee_paid;
    std::string_view fee_currency;
    std::string_view fill_type; // maker | taker | liquidation | ...
    std::uint64_t time_ms{0};
    bool buy{false};
};

struct WsFillBuffer {
    std::array<WireWsFill, k_max_fills> fills{};
};

// Reads `fills` and `fills_snapshot` through one path: both carry the same
// array under the same key.
[[nodiscard]] std::expected<std::span<const WireWsFill>, WireError>
parse_ws_fills(std::string_view frame, WsFillBuffer& storage);

} // namespace raikiri::venues::kraken
