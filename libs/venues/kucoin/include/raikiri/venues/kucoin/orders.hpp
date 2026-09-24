#pragma once

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// one topic carries the whole lifecycle; filled is not another fill
enum class OrderChangeType : std::uint8_t {
    Open,
    Match,
    Filled,
    Canceled,
    Update, // a modify took effect; `old_size` names what it replaced
    Unknown,
};

// sizes are decimals on spot and contracts on futures; order_time is quoted on futures
struct WireOrderChange {
    OrderChangeType type{OrderChangeType::Unknown};
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view trade_id;   // `match` only; the fill's dedupe key
    std::string_view side;       // "buy" / "sell"
    std::string_view order_type; // "limit" / "market"
    std::string_view price;      // the order's price; "0" on a market order
    std::string_view size;
    std::string_view filled_size;
    std::string_view match_price; // `match` only, and the price that traded
    std::string_view match_size;  // `match` only
    std::string_view remain_size;
    std::string_view canceled_size;
    std::string_view old_size;  // `update` only
    std::string_view liquidity; // "maker" / "taker"; futures states it, spot does not
    // milliseconds on spot, nanoseconds on futures; ts is nanoseconds on both
    std::uint64_t order_time{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireOrderChange, WireError> parse_order_change(std::string_view frame);

// one row for both hosts; remain_size is spot-only
struct WireOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view side;
    std::string_view order_type;
    std::string_view price;
    std::string_view size;
    std::string_view deal_size;
    std::string_view remain_size;
    std::string_view status; // futures "open" / "done"; absent on spot
    bool active{false};
    bool cancel_exist{false};
    std::uint64_t updated_ms{0};
};

inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// data is the array itself
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_spot_orders(std::string_view body, OrderBuffer& storage);

// data is a page object wrapping items
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_futures_orders(std::string_view body, OrderBuffer& storage);

inline constexpr std::size_t k_max_active_symbols = 256;

struct SymbolBuffer {
    std::array<std::string_view, k_max_active_symbols> symbols{};
};

// spot answers actives one symbol at a time; this lists the symbols
[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_active_symbols(std::string_view body, SymbolBuffer& storage);

// neither host names the clientOid on a fill
struct WireFill {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view side;
    std::string_view liquidity; // "maker" / "taker"
    std::string_view price;
    std::string_view size;
    std::string_view fee;
    std::uint64_t created_ms{0};
};

inline constexpr std::size_t k_max_fills = 512;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

// GET /api/v1/fills and GET /api/v1/hf/fills: one shape, both hosts
[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

} // namespace raikiri::venues::kucoin
