#pragma once

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// sendorder, cancelorder and editorder answer the same object under different keys
enum class StatusKind : std::uint8_t { Send, Cancel, Edit };

// status is the only verdict; the envelope says success for refused orders too
struct WireOrderStatus {
    std::string_view status;
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::uint64_t received_ms{0};
};

[[nodiscard]] std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body, StatusKind kind);

// batchStatus answers one status per element in request order
inline constexpr std::size_t k_max_batch = 50;

struct BatchStatusBuffer {
    std::array<WireOrderStatus, k_max_batch> statuses{};
};

[[nodiscard]] std::expected<std::span<const WireOrderStatus>, WireError> parse_batch_status(std::string_view body,
                                                                                            BatchStatusBuffer& storage);

// unfilledSize is what is left and filledSize what traded; the original size is not stated
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

[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError> parse_open_orders(std::string_view body,
                                                                                         OpenOrderBuffer& storage);

// fill_time is kept as text to echo as the lastFillTime cursor
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

// the endpoint answers at most 100 rows per page
inline constexpr std::size_t k_max_fills = 256;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

// qty is what remains; filled is cumulative
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

// a removal carries only order_id at the root, so the instrument comes from local memory
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

[[nodiscard]] std::expected<std::span<const WireWsOrder>, WireError> parse_order_snapshot(std::string_view frame,
                                                                                          OrderSnapshotBuffer& storage);

// fee_paid is signed, in base on PI_ and quote on PF_
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

// fills and fills_snapshot share one shape
[[nodiscard]] std::expected<std::span<const WireWsFill>, WireError> parse_ws_fills(std::string_view frame,
                                                                                   WsFillBuffer& storage);

} // namespace raikiri::venues::kraken
