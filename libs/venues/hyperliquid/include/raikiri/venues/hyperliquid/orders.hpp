#pragma once

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// frontendOpenOrders adds the frontend-only fields; only `tif` says whether an order is post-only
struct WireOpenOrder {
    std::string_view coin{};
    std::string_view price{};      // limitPx
    std::string_view qty{};        // sz, remaining
    std::string_view orig_qty{};   // origSz
    std::string_view cloid{};      // absent entirely on an order placed without one
    std::string_view order_type{}; // frontend only: "Limit", "Stop Market", ...
    std::string_view tif{};        // frontend only: "Gtc" | "Alo" | "Ioc" | "FrontendMarket"
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

[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError> parse_open_orders(std::string_view body,
                                                                                         OpenOrderBuffer& storage);

// `crossed` true means taker; `fee` is negative on a maker rebate
struct WireFill {
    std::string_view coin{};
    std::string_view price{}; // px
    std::string_view qty{};   // sz
    std::string_view fee{};
    std::string_view fee_token{};
    std::string_view cloid{}; // absent on a fill of an order placed without one
    std::uint64_t oid{0};
    std::uint64_t tid{0};
    std::uint64_t ts_ms{0};
    bool buy{false};
    bool crossed{false};
};

inline constexpr std::size_t k_max_fills = 2048;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

// an unknown oid answers {"status":"unknownOid"}, which is not an error
struct WireOrderStatus {
    WireOpenOrder order{};
    std::string_view status{}; // "open" | "filled" | "canceled" | "triggered" | ...
    std::uint64_t ts_ms{0};
    bool found{false};
};

[[nodiscard]] std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body);

enum class ExchangeOutcome : std::uint8_t {
    Resting,           // accepted and in the book
    Filled,            // matched on arrival, wholly or partly
    Success,           // a cancel or modify the venue performed
    WaitingForTrigger, // a conditional order armed; arrives as a bare string
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

inline constexpr std::size_t k_max_statuses = 64;

struct ExchangeStatusBuffer {
    std::array<WireExchangeStatus, k_max_statuses> statuses{};
};

// a refused order is status "ok" with an `error` in `statuses`;
// `response` is a string on top-level failure
struct WireExchangeReply {
    bool ok{false};
    std::string_view message; // top-level prose, when !ok
    std::string_view type;    // "order" | "cancel" | "default"
    std::span<const WireExchangeStatus> statuses;
};

[[nodiscard]] std::expected<WireExchangeReply, WireError> parse_exchange_reply(std::string_view body,
                                                                               ExchangeStatusBuffer& storage);

} // namespace raikiri::venues::hyperliquid
