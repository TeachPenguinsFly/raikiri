#pragma once

#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::aster {

struct WireInstrument {
    std::string_view symbol;
    std::string_view base_asset; // aster names both halves on every row
    std::string_view quote_asset;
    std::string_view contract_type; // PERPETUAL, CURRENT_QUARTER, ...
    std::string_view status;        // TRADING, SETTLING, PENDING_TRADING, ...
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_notional;
    std::uint64_t delivery_ms{0};
    std::int64_t symbol_type{0};          // 1 on a tradfi row
    std::string_view underlying_sub_type; // raw array: "STOCK", "ETF", "Commodities", ...
    // stamped from /fapi/v1/fundingInfo; 0 means aster did not state it
    std::uint32_t funding_interval_s{0};
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

// a symbol missing here has no stated schedule
struct WireFundingInfo {
    std::string_view symbol;
    std::uint32_t interval_hours{0};
    // per-interval decimal rate strings
    std::string_view cap;
    std::string_view floor;
};

inline constexpr std::size_t k_max_funding_info = k_max_instruments;

struct FundingInfoBuffer {
    std::array<WireFundingInfo, k_max_funding_info> rows{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError> parse_exchange_info(std::string_view body,
                                                                                            InstrumentBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireFundingInfo>, WireError> parse_funding_info(std::string_view body,
                                                                                            FundingInfoBuffer& storage);

} // namespace raikiri::venues::aster
