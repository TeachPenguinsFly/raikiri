#pragma once

// private frames arrive on channel 0 tagged os/on/ou/oc, te/tu and n; rest returns the same arrays
// signed amounts carry the side; quantities stay positive

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// the side comes from amount_orig; amount is what remains and is 0 once filled
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

// status is prose with the fill appended, so it is matched by prefix
enum class WireOrderStatus : std::uint8_t {
    Active,
    PartiallyFilled,
    Executed,
    Canceled, // includes postonly canceled
    Rejected, // insufficient margin or balance, rsn_*
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status(std::string_view text) noexcept;

// order flags bitfield
inline constexpr std::int64_t k_flag_hidden = 64;
inline constexpr std::int64_t k_flag_close = 512;
inline constexpr std::int64_t k_flag_reduce_only = 1024;
inline constexpr std::int64_t k_flag_post_only = 4096;
inline constexpr std::int64_t k_flag_oco = 16384;

struct WireOrder {
    std::string_view id;
    std::string_view symbol;
    std::string_view amount; // signed; what remains
    std::string_view amount_orig; // signed; the reliable side
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

inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// the on/ou/oc push payload or a rest single-order row
[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view frame);

// the os snapshot and every rest order read
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// exec_amount's sign is our side; maker is 1 or -1
// te and tu are the same fill, te without the fee; emit only one
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
    std::string_view amount; // signed; the sign is our side
    std::string_view price;
    std::string_view fee;          // empty on `te`, where it is not yet known
    std::string_view fee_currency; // likewise
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t mts{0};
};

inline constexpr std::size_t k_max_executions = 512;

struct ExecutionBuffer {
    std::array<WireExecution, k_max_executions> executions{};
};

[[nodiscard]] std::expected<WireExecution, WireError> parse_execution(std::string_view frame);

[[nodiscard]] std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view body, ExecutionBuffer& storage);

// the answer to every write; status is the verdict since a refusal still gets http 200
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
