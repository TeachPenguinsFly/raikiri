#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/venues/blofin/catalogue.hpp>
#include <raikiri/venues/blofin/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::blofin {

struct WirePrint {
    std::string_view inst_id;
    std::string_view price;
    std::string_view size;
    bool taker_bought{false}; // `side` is the taker's
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_prints = 128;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError>
parse_trades(std::string_view frame, PrintBuffer& storage);

struct WireTicker {
    std::string_view inst_id;
    std::string_view vol_contracts; // vol24h
    std::string_view vol_currency;  // volCurrency24h: base on linear, contracts again on inverse
    std::string_view last;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_tickers = 16;

struct TickerBuffer {
    std::array<WireTicker, k_max_tickers> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError>
parse_tickers(std::string_view frame, TickerBuffer& storage);

// A row of /api/v1/market/funding-rate, or of the funding-rate channel, which
// carries the same row without the period, the cap and the floor.
struct WireFundingRow {
    std::string_view inst_id;
    std::string_view funding_rate;
    std::uint64_t funding_ms{0}; // the next settlement
    std::string_view interval;
    std::string_view interval_unit; // hour | minute
    std::string_view cap;
    std::string_view floor;
};

struct FundingBuffer {
    std::array<WireFundingRow, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFundingRow>, WireError>
parse_funding(std::string_view body, FundingBuffer& storage);

struct WireMark {
    std::string_view inst_id;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

struct MarkBuffer {
    std::array<WireMark, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMark>, WireError>
parse_mark_table(std::string_view body, MarkBuffer& storage);

// The record is written whole, but its fields arrive from three sources; each updates its own.
struct FundingMemo {
    FixedE9 rate{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::blofin
