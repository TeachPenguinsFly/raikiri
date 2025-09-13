#pragma once

// An order's life on the wire: the futures personal pushes and REST lists, and
// the spot REST reads that stand in for the spot socket MEXC no longer serves
// as JSON.
//
// TWO DISJOINT SCHEMAS LIVE HERE, and nothing may share a parser between them.
// Futures state an order with numeric fields (`vol`, `dealVol`, integer
// `side`, integer `state`); spot states the same order the binance way with
// quoted decimals and SCREAMING_SNAKE statuses. A reader that guesses reads a
// close order as an open one.

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// Resolved once per record so the mapper branches on an integer. MEXC futures
// collapses New and PartiallyFilled into state 2 and only the filled volume
// tells them apart, which is why that split happens in the mapper and not
// here.
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

// One private record is one order; a REST list can carry a page of them.
inline constexpr std::size_t k_max_private_records = 512;

// Futures --------------------------------------------------------------

// `order_id` reads `orderId` on REST and `id` on the socket — the same value
// under two names, which is the one place the two futures schemas disagree.
// `side` is the four-valued OPEN/CLOSE enum, not a direction: 1 open long,
// 2 close short, 3 open short, 4 close long.
struct WireFuturesOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view external_oid; // our ULID, echoed back
    std::string_view price;
    std::string_view vol;      // CONTRACTS ordered
    std::string_view deal_vol; // CONTRACTS filled so far
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

// `taker` on REST, `isTaker` on the socket — read both, because a fill with
// the liquidity flag lost is a maker strategy with no way to score itself.
struct WireFuturesDeal {
    std::string_view symbol;
    std::string_view deal_id;
    std::string_view order_id;
    std::string_view price;
    std::string_view vol; // CONTRACTS
    std::string_view fee;
    std::int64_t side{0};
    bool taker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct FuturesDealBuffer {
    std::array<WireFuturesDeal, k_max_private_records> deals{};
};

// `data` as an ARRAY (the REST list endpoints).
[[nodiscard]] std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage);

// `data` as a single OBJECT: the socket push, and the REST single-order reads.
[[nodiscard]] std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage);

[[nodiscard]] std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame);

// Spot -----------------------------------------------------------------

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

// `is_buyer` is the ACCOUNT's side, not the aggressor's — the opposite of what
// the public deal channel's flag means, and reading it that way inverts the
// side of every reconciled fill.
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

// The spot bodies are BARE arrays and objects: there is no envelope to peel,
// which is the second reason spot and futures cannot share a reader.
[[nodiscard]] std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage);

[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage);

} // namespace raikiri::venues::mexc
