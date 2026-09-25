#pragma once

// one row reader serves the socket and both rest lists, which spell the same fields differently

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// spot spells cancelled with two ls and mix with one; read both
enum class WireOrderStatus : std::uint8_t {
    Live,
    PartiallyFilled,
    Filled,
    Canceled,
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_from(std::string_view text) noexcept;

// fills arrive inline on orders, so the separate fill channel is not subscribed
struct WireOrderUpdate {
    std::string_view inst_id;
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view side;       // buy, sell
    std::string_view order_type; // limit, market
    std::string_view force;      // gtc, post_only, fok, ioc
    std::string_view price;
    // spot states a market buy's size in the quote coin; new_size is base
    std::string_view size;
    std::string_view new_size;
    std::string_view acc_base_volume; // cumulative filled, base units
    std::string_view base_volume;     // this fill alone
    std::string_view fill_price;
    std::string_view fill_fee;
    std::string_view trade_id;
    std::string_view trade_scope; // "T"/"M" here; the `fill` channel spells it out
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t fill_ms{0};
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_updates = 128;

struct OrderUpdateBuffer {
    std::array<WireOrderUpdate, k_max_updates> updates{};
};

[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError>
parse_order_updates(std::string_view frame, OrderUpdateBuffer& storage);

// data is the array, for orderInfo too
[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError> parse_spot_orders(std::string_view body,
                                                                                           OrderUpdateBuffer& storage);

// the array is under data.entrustedList
[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError> parse_mix_orders(std::string_view body,
                                                                                          OrderUpdateBuffer& storage);

struct WireFill {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view trade_id;
    std::string_view side;
    std::string_view price;
    std::string_view size;
    std::string_view fee;         // feeDetail totalFee; negative on Bitget
    std::string_view trade_scope; // taker, maker
    std::uint64_t ts_ms{0};
};

struct FillBuffer {
    std::array<WireFill, k_max_updates> fills{};
};

// data is the array; feeDetail is an object
[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_spot_fills(std::string_view body,
                                                                                   FillBuffer& storage);

// data.fillList; feeDetail is an array
[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_mix_fills(std::string_view body,
                                                                                  FillBuffer& storage);

// order_id may be empty when a reduce-only order displaces others; clientOid is then the handle
struct WireOrderAck {
    std::string_view order_id;
    std::string_view client_oid;
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body);

// batch cancels succeed at the envelope while refusing orders in failureList
struct WireBatchFailure {
    std::string_view order_id;
    std::string_view client_oid;
    std::string_view code;
    std::string_view msg;
};

inline constexpr std::size_t k_max_cancel_rows = 50;

struct BatchFailureBuffer {
    std::array<WireBatchFailure, k_max_cancel_rows> failures{};
};

[[nodiscard]] std::expected<std::span<const WireBatchFailure>, WireError>
parse_batch_failures(std::string_view body, BatchFailureBuffer& storage);

} // namespace raikiri::venues::bitget
