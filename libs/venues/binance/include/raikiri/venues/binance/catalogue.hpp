#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// The three sizes come off the row's `filters` array rather than off named
// fields, which is why the reader walks it once per row.
struct WireInstrument {
    std::string_view symbol;
    std::string_view base_asset; // binance names both halves on every row
    std::string_view quote_asset;
    std::string_view contract_type; // PERPETUAL, CURRENT_QUARTER, ...
    std::string_view status;        // TRADING, SETTLING, PENDING_TRADING, ...
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_notional;
    std::uint64_t delivery_ms{0};
    // NOT FROM exchangeInfo — stamped on afterwards from /fapi/v1/fundingInfo,
    // the only place binance states it. 0 means the schedule is unknown, which
    // downstream must read as "unstated" and never as eight hours.
    std::uint32_t funding_interval_s{0};
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

// One row of /fapi/v1/fundingInfo. The endpoint answers every trading perp —
// 571 of 571 when this was written — so a symbol missing from it is a symbol
// whose schedule binance did not state, not one that settles on a default.
struct WireFundingInfo {
    std::string_view symbol;
    std::uint32_t interval_hours{0};
    // adjustedFundingRateCap / adjustedFundingRateFloor, as binance writes
    // them: decimal strings of a per-interval rate, not an annual one.
    std::string_view cap;
    std::string_view floor;
};

inline constexpr std::size_t k_max_funding_info = k_max_instruments;

struct FundingInfoBuffer {
    std::array<WireFundingInfo, k_max_funding_info> rows{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_exchange_info(std::string_view body, InstrumentBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireFundingInfo>, WireError>
parse_funding_info(std::string_view body, FundingInfoBuffer& storage);

} // namespace raikiri::venues::binance
