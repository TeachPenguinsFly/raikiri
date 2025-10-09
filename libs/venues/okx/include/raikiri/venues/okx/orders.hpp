#pragma once

// An order's life on the wire: what the channel says happened to it, what a
// REST answer says about it, and the fills it produced.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// OKX's order lifecycle. mmp_canceled is a market-maker-protection cancel:
// terminal, and not a fill however much the name reads like one.
enum class WireOrderState : std::uint8_t {
    Live,
    PartiallyFilled,
    Filled,
    Canceled,
    MmpCanceled,
    Unknown,
};

[[nodiscard]] WireOrderState order_state_from(std::string_view text) noexcept;

// One element of the private `orders` channel, which is also the shape
// GET /trade/orders-pending returns.
//
// An element is BOTH a fill and the state change that fill caused, and
// `trade_id` non-empty is OKX's own marker for "this update carries a
// trade". A reader that maps state only loses every fill; one that maps
// fills only loses every cancel. OKX documents that the fill_* fields carry
// DIFFERENT meanings on the REST order endpoints than on this channel, so
// nothing may read them off a REST body.
struct WireOrder {
    std::string_view inst_id;
    std::string_view ord_id;
    std::string_view cl_ord_id; // our ULID comes back here
    std::string_view side;      // "buy" | "sell"
    std::string_view ord_type;
    std::string_view px;
    std::string_view sz;          // the order's total size, never the remainder
    std::string_view acc_fill_sz; // cumulative; leaves = sz - acc_fill_sz
    std::string_view avg_px;
    std::string_view fill_px;
    std::string_view fill_sz;
    std::string_view fill_fee; // NEGATIVE is a fee charged, positive a rebate
    std::string_view trade_id;
    std::string_view exec_type;     // "T" taker, "M" maker
    std::string_view cancel_source; // "13"/"14": an FOK/IOC remainder expiring
    std::string_view amend_result;  // "-1": the venue refused an amend
    WireOrderState state{WireOrderState::Unknown};
    std::int64_t code{0}; // per-order failure; 0 on every healthy push
    std::uint64_t fill_ms{0};
    std::uint64_t u_ms{0};
};

// orders-pending pages at 100; the channel pushes one at a time.
inline constexpr std::size_t k_max_orders = 128;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// One element of GET /api/v5/trade/fills. `fee` here is the REST spelling of
// the channel's fill_fee and carries the same sign convention.
struct WireFill {
    std::string_view inst_id;
    std::string_view trade_id;
    std::string_view ord_id;
    std::string_view cl_ord_id;
    std::string_view side;
    std::string_view fill_px;
    std::string_view fill_sz;
    std::string_view fee;
    std::string_view exec_type;
    std::uint64_t fill_ms{0};
};

inline constexpr std::size_t k_max_fills = 128;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

// One element of data[] on order / cancel-order / amend-order / the batch
// endpoints. The envelope's `code` reports only whether the BATCH went
// through: a single order OKX refuses answers code "1" — "Operation failed" —
// with the actual reason in sCode. A caller that reads only the envelope
// classifies every rejection as the same generic failure and can never tell
// "insufficient balance" from "post-only would have crossed".
struct WireOrderAck {
    std::string_view ord_id;
    std::string_view cl_ord_id;
    std::int64_t s_code{0};
    std::string_view s_msg;
};

// The batch endpoints cap at 20 entries; the ceiling leaves headroom.
inline constexpr std::size_t k_max_acks = 32;

struct AckBuffer {
    std::array<WireOrderAck, k_max_acks> acks{};
};

[[nodiscard]] std::expected<std::span<const WireOrderAck>, WireError>
parse_order_acks(std::string_view body, AckBuffer& storage);

} // namespace raikiri::venues::okx
