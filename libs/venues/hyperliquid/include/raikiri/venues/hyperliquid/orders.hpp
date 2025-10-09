#pragma once

// An order's life on the wire: what /info says is resting, what it filled, and
// what /exchange said about the write that put it there.

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// A row of openOrders, or of frontendOpenOrders — which is the same row plus
// the fields marked below. The plain form leaves them empty/false, and a
// caller that needs to know whether a resting order is post-only must ask the
// frontend form: `tif` is the only place "Alo" appears.
struct WireOpenOrder {
    std::string_view coin;
    std::string_view price;      // limitPx
    std::string_view qty;        // sz — REMAINING, not original
    std::string_view orig_qty;   // origSz
    std::string_view cloid;      // absent entirely on an order placed without one
    std::string_view order_type; // frontend only: "Limit", "Stop Market", ...
    std::string_view tif;        // frontend only: "Gtc" | "Alo" | "Ioc" | "FrontendMarket"
    std::uint64_t oid{0};
    std::uint64_t ts_ms{0};
    bool buy{false};         // `side`: "B" bids, "A" asks
    bool reduce_only{false}; // frontend only
    bool is_trigger{false};  // frontend only
};

inline constexpr std::size_t k_max_open_orders = 1024;

struct OpenOrderBuffer {
    std::array<WireOpenOrder, k_max_open_orders> orders{};
};

// Both forms parse here: the extra fields are read when present. The body is a
// bare ARRAY, not an envelope.
[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view body, OpenOrderBuffer& storage);

// A row of userFills / userFillsByTime, newest first.
//
// `crossed` is the liquidity flag: true means we took. `fee` is NEGATIVE when
// the venue paid us a maker rebate, so it must not be read as an unsigned
// magnitude.
struct WireFill {
    std::string_view coin;
    std::string_view price; // px
    std::string_view qty;   // sz
    std::string_view fee;
    std::string_view fee_token;
    std::string_view cloid; // absent on a fill of an order placed without one
    std::uint64_t oid{0};
    std::uint64_t tid{0};
    std::uint64_t ts_ms{0};
    bool buy{false};
    bool crossed{false};
};

// The venue caps a userFills answer at 2000 rows.
inline constexpr std::size_t k_max_fills = 2048;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

// {"status":"order","order":{"order":{...},"status":"filled","statusTimestamp":..}}
// or the bare {"status":"unknownOid"} — VERIFIED live, and the reason `found`
// exists: an order id the venue has forgotten and one it never minted answer
// identically, and neither is an error.
struct WireOrderStatus {
    WireOpenOrder order{};
    std::string_view status; // "open" | "filled" | "canceled" | "triggered" | ...
    std::uint64_t ts_ms{0};
    bool found{false};
};

[[nodiscard]] std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body);

// What one entry of an /exchange reply says happened to one order.
enum class ExchangeOutcome : std::uint8_t {
    Resting,           // accepted and in the book
    Filled,            // matched on arrival, wholly or partly
    Success,           // a cancel or modify the venue performed
    WaitingForTrigger, // a conditional order armed; arrives as a bare STRING
    Error,             // per-order refusal, `error` carries the prose
    Unknown,
};

struct WireExchangeStatus {
    ExchangeOutcome outcome{ExchangeOutcome::Unknown};
    std::string_view error;     // prose; the protocol has no numeric code
    std::string_view avg_price; // filled only
    std::string_view total_qty; // filled only
    std::uint64_t oid{0};
};

// One action per request today; the array exists because the venue's shape is
// a batch and a future batch must not silently keep only its first answer.
inline constexpr std::size_t k_max_statuses = 64;

struct ExchangeStatusBuffer {
    std::array<WireExchangeStatus, k_max_statuses> statuses{};
};

// A REFUSED order is `status: "ok"` with an `error` inside `statuses` — only a
// malformed or unauthorised REQUEST reaches `status: "err"`. Reading the outer
// status alone therefore reports every rejected order as accepted.
//
// `response` is the same key with two types: an OBJECT on success and a plain
// STRING carrying prose on a top-level failure.
struct WireExchangeReply {
    bool ok{false};
    std::string_view message; // top-level prose, when !ok
    std::string_view type;    // "order" | "cancel" | "default"
    std::span<const WireExchangeStatus> statuses;
};

[[nodiscard]] std::expected<WireExchangeReply, WireError>
parse_exchange_reply(std::string_view body, ExchangeStatusBuffer& storage);

} // namespace raikiri::venues::hyperliquid
