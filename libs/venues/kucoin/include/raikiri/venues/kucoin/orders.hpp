#pragma once

// An order's life on the wire: what the private topic says happened to it,
// what a REST answer says about it, and the fills it produced.

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// KuCoin puts the WHOLE order lifecycle on one topic per market family: there
// is no separate execution channel the way bybit has, and an ack, a fill and a
// cancel differ only by `type`.
//
// `Filled` is the venue saying the order is done; the trades that filled it
// already arrived as `Match`. Treating it as another fill double-counts the
// last one.
enum class OrderChangeType : std::uint8_t {
    Open,
    Match,
    Filled,
    Canceled,
    Update, // a modify took effect; `old_size` names what it replaced
    Unknown,
};

// Sizes are decimal text on spot and CONTRACTS on futures, and `order_time` is
// a JSON number on spot but a quoted string on futures — both reach the DTO as
// the raw token, so the mapper divides by the instrument's own steps either way.
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
    // `order_time` is MILLISECONDS on spot and NANOSECONDS on futures — the
    // same field name, two scales, six orders of magnitude apart. `ts` is
    // nanoseconds on both, so it is the one the mapper times events by.
    std::uint64_t order_time{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireOrderChange, WireError> parse_order_change(std::string_view frame);

// One row shape for both hosts: the spot and futures order lists disagree on
// which fields they send, not on what the shared ones are called. `remain_size`
// is spot-only — futures states `deal_size` and leaves the subtraction to the
// mapper.
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

// GET /api/v1/hf/orders/active — `data` is the array itself.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_spot_orders(std::string_view body, OrderBuffer& storage);

// GET /api/v1/orders?status=active — `data` is a page object wrapping `items`.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_futures_orders(std::string_view body, OrderBuffer& storage);

inline constexpr std::size_t k_max_active_symbols = 256;

struct SymbolBuffer {
    std::array<std::string_view, k_max_active_symbols> symbols{};
};

// GET /api/v1/hf/orders/active/symbols. The first half of the spot sweep:
// KuCoin answers actives one symbol at a time.
[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_active_symbols(std::string_view body, SymbolBuffer& storage);

// Neither host names the clientOid on a fill, so a fill is attributed through
// its `order_id` alone.
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

// GET /api/v1/fills and GET /api/v1/hf/fills — one shape, both hosts.
[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

} // namespace raikiri::venues::kucoin
