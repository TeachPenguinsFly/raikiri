#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

struct WireInstrument {
    std::string_view symbol;
    std::string_view base_coin;
    std::string_view quote_coin;
    // LinearPerpetual, LinearFutures, InversePerpetual, InverseFutures; the only field telling perps from futures
    std::string_view contract_type;
    std::string_view symbol_type; // "", "innovation", "stock", "ETF", "commodity", "forex"
    std::string_view tick_size;
    std::string_view qty_step;
    std::string_view min_order_qty;
    std::string_view min_notional;
    // minutes, as a json number; 0 means unstated
    std::uint64_t funding_interval_min{0};
    // upperFundingRate / lowerFundingRate per interval; varies by symbol
    std::string_view funding_cap;
    std::string_view funding_floor;
    std::string_view status; // "Trading" for live instruments
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view result,
                                                                                          InstrumentBuffer& storage);

// nextPageCursor, already percent-encoded; empty on the last page.
[[nodiscard]] std::expected<std::string_view, WireError> parse_page_cursor(std::string_view result);

} // namespace raikiri::venues::bybit
