#pragma once

// futures and spot order schemas differ and share no parser

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// futures state 2 covers new and partially filled; the mapper splits them by filled volume
enum class WireOrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
    Expired,
    Unknown,
};

[[nodiscard]] WireOrderStatus spot_order_status(std::string_view text) noexcept;

inline constexpr std::size_t k_max_private_records = 512;

// `orderId` on rest, `id` on the socket; `side` is 1 open long,
// 2 close short, 3 open short, 4 close long
struct WireFuturesOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view external_oid; // our ulid, echoed back
    std::string_view price;
    std::string_view vol;      // contracts ordered
    std::string_view deal_vol; // contracts filled so far
    std::string_view deal_avg_price;
    std::int64_t side{0};
    std::int64_t order_type{0};
    std::int64_t state{0};      // 1 pending, 2 working, 3 filled, 4 cancelled, 5 invalid
    std::int64_t error_code{0}; // non-zero on a venue-side refusal
    std::uint64_t update_ms{0};
};

struct FuturesOrderBuffer {
    std::array<WireFuturesOrder, k_max_private_records> orders{};
};

// `taker` on rest, `isTaker` on the socket
struct WireFuturesDeal {
    std::string_view symbol;
    std::string_view deal_id;
    std::string_view order_id;
    std::string_view price;
    std::string_view vol; // contracts
    std::string_view fee;
    std::int64_t side{0};
    bool taker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct FuturesDealBuffer {
    std::array<WireFuturesDeal, k_max_private_records> deals{};
};

// `data` as an array (rest lists)
[[nodiscard]] std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage);

// `data` as a single object: the socket push and single-order rest reads
[[nodiscard]] std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage);

[[nodiscard]] std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame);

struct WireSpotOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::string_view side; // BUY | SELL
    std::string_view type; // LIMIT | MARKET | LIMIT_MAKER
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t update_ms{0};
};

struct SpotOrderBuffer {
    std::array<WireSpotOrder, k_max_private_records> orders{};
};

// `is_buyer` is the account's side, not the aggressor's
struct WireSpotTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view qty;
    std::string_view commission;
    bool is_buyer{false};
    bool is_maker{false};
    std::uint64_t ts_ms{0};
};

struct SpotTradeBuffer {
    std::array<WireSpotTrade, k_max_private_records> trades{};
};

// spot bodies have no envelope
[[nodiscard]] std::expected<std::span<const WireSpotOrder>, WireError> parse_spot_orders(std::string_view body,
                                                                                         SpotOrderBuffer& storage);

[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError> parse_spot_trades(std::string_view body,
                                                                                         SpotTradeBuffer& storage);

} // namespace raikiri::venues::mexc
