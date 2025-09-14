#pragma once

// An order's life on the wire, in TWO DIALECTS, and nothing may share a reader
// between them.
//
// Futures states an order with long names on BOTH transports (`symbol`,
// `origQty`, `orderSide`, `state`); spot states the same order with long names
// over REST and with TWO-LETTER names over the socket (`s`, `oq`, `sd`, `st`).
// A reader that guessed would read a resting order's price as its quantity.
//
// Numbers arrive in both encodings for the same field — /order/list ships
// `price` as a bare JSON number while the socket quotes it — so every decimal
// below is read with opt_scalar, which keeps the token either way.

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// One state, resolved once per record so the mapper branches on an integer.
// PARTIALLY_CANCELED is a cancel that took volume first: the fill was already
// reported by its own trade, so the lifecycle event is a cancel.
enum class WireOrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
    Expired,
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status(std::string_view text) noexcept;

// A page of orders, deals or trades. XT caps `size` at 100 per read, so a
// buffer this size can hold several pages without ever growing.
//
// Its own, though it matches the position and balance buffers': all four used
// to borrow one k_max_private_records, which made a change to any of them
// silently resize the other three.
inline constexpr std::size_t k_max_orders = 512;

// Futures ---------------------------------------------------------------
//
// `positionSide` is LONG/SHORT and is NOT the direction: a SELL that closes a
// long carries positionSide LONG. `orderSide` is the direction, and it is the
// only field that is.
struct WireFuturesOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view orig_qty;     // CONTRACTS
    std::string_view executed_qty; // CONTRACTS
    std::string_view avg_price;
    std::string_view order_side;    // BUY | SELL
    std::string_view position_side; // LONG | SHORT
    std::string_view order_type;    // LIMIT | MARKET
    std::string_view time_in_force; // GTC | IOC | FOK | GTX
    WireOrderStatus state{WireOrderStatus::Unknown};
    std::uint64_t created_ms{0};
    std::uint64_t updated_ms{0};
};

struct FuturesOrderBuffer {
    std::array<WireFuturesOrder, k_max_orders> orders{};
};

// The two transports disagree about liquidity and about the trade id:
// /order/trade-list ships `execId` plus `takerMaker` as TEXT, the socket ships
// neither the id nor that field and says `isMaker` as a BOOLEAN instead. Both
// are read; `liquidity_known` says whether either was present, because a fill
// whose side of the book is guessed makes a maker strategy score itself
// against a number it invented.
struct WireFuturesDeal {
    std::string_view symbol;
    std::string_view exec_id; // empty on the socket push
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view quantity; // CONTRACTS
    std::string_view fee;
    std::string_view order_side; // empty on /order/trade-list
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct FuturesDealBuffer {
    std::array<WireFuturesDeal, k_max_orders> deals{};
};

// `result` as an ARRAY, or as `result.items` — the paged reads wrap the list
// in a page object and the unpaged ones do not.
[[nodiscard]] std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage);

// `data` on the socket push, `result` on the single-order REST read.
[[nodiscard]] std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage);

[[nodiscard]] std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame);

// The create/cancel/amend answers, whose `result` is the venue order id — a
// BARE STRING on futures create and cancel, an object with `orderId` on the
// futures amend and on every spot write. Empty when the venue named none.
[[nodiscard]] std::expected<std::string_view, WireError> parse_order_ack(std::string_view body);

// Spot ------------------------------------------------------------------

struct WireSpotOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view side; // BUY | SELL
    std::string_view type; // LIMIT | MARKET
    std::string_view time_in_force;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::string_view leaving_qty; // XT states leaves outright; nothing is derived
    std::string_view avg_price;
    std::string_view fee;
    WireOrderStatus state{WireOrderStatus::Unknown};
    std::uint64_t ts_ms{0};
};

struct SpotOrderBuffer {
    std::array<WireSpotOrder, k_max_orders> orders{};
};

// `takerMaker` is the ACCOUNT's side of the book, and on the socket push it
// arrives as `tm`: 1 taker, 2 maker.
struct WireSpotTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view side; // BUY | SELL; empty on the socket push
    std::string_view price;
    std::string_view quantity;
    std::string_view fee;
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct SpotTradeBuffer {
    std::array<WireSpotTrade, k_max_orders> trades{};
};

// REST: `result` as an array (open-order) or `result.items` (history/trade).
[[nodiscard]] std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage);

[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage);

// The socket pushes, whose keys are the two-letter set.
[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order_push(std::string_view frame);

[[nodiscard]] std::expected<WireSpotTrade, WireError> parse_spot_trade_push(std::string_view frame);

} // namespace raikiri::venues::xt
