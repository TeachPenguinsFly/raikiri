#pragma once

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/fill_report.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/domain/position_report.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitunix/wire.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::bitunix {

struct WireAck {
    std::string_view order_id;
    std::string_view client_id;
};

[[nodiscard]] std::expected<WireAck, WireError> parse_ack(std::string_view body);

// a cancel answers 200 per order; the failure list says which did not go
struct WireCancel {
    WireAck ack;
    bool ok{false};
    std::int64_t error_code{0};
    std::string_view error;
};

inline constexpr std::size_t k_max_rows = 512;

struct CancelBuffer {
    std::array<WireCancel, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireCancel>, WireError> parse_cancels(std::string_view body,
                                                                                  CancelBuffer& storage);

struct WireOrder {
    std::string_view order_id;
    std::string_view client_id;
    std::string_view symbol;
    std::string_view side;
    std::string_view status;
    std::string_view price;
    std::string_view qty;
    std::string_view filled;
    std::uint64_t mtime_ms{0};
};

struct OrderBuffer {
    std::array<WireOrder, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                                OrderBuffer& storage);

struct WireFill {
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view client_id;
    std::string_view symbol;
    std::string_view side;
    std::string_view price;
    std::string_view qty;
    std::string_view fee;
    std::string_view role;
    std::uint64_t ctime_ms{0};
};

struct FillBuffer {
    std::array<WireFill, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

struct WirePosition {
    std::string_view symbol;
    std::string_view side;
    std::string_view qty;
    std::string_view avg_price;
    std::uint64_t mtime_ms{0};
};

struct PositionBuffer {
    std::array<WirePosition, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

struct WireBalance {
    std::string_view coin;
    std::string_view available;
    std::string_view frozen;
    std::string_view margin;
    std::string_view cross_unrealized;
    std::string_view isolated_unrealized;
};

struct BalanceBuffer {
    std::array<WireBalance, 16> rows{};
};

[[nodiscard]] std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body,
                                                                                    BalanceBuffer& storage);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_order(const WireOrder& wire,
                                                               const exec::InstrumentTable& table);

[[nodiscard]] std::optional<FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table);

// one-way mode states a short as a positive qty on side SHORT
[[nodiscard]] std::optional<PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos ts);

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

} // namespace raikiri::venues::bitunix
