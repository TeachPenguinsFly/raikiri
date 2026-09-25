#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    bool buyer_aggressor{false}; // "S":"Buy" means the taker bought
    std::uint64_t ts_ms{0};
    std::string_view trade_id;
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    WireTrade trades[k_max_trades];
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

// a snapshot, then deltas of changed fields only; absent means unchanged
struct WireTicker {
    std::string_view symbol;
    bool is_snapshot{false};
    std::string_view mark_price; // empty when absent
    std::string_view index_price;
    std::string_view funding_rate;
    std::string_view next_funding_ms;
    // empty on a delta that did not change it; empty is not zero
    std::string_view volume_24h;   // base units
    std::string_view turnover_24h; // quote units
    std::string_view bid1_price;
    std::string_view bid1_size;
    std::string_view ask1_price;
    std::string_view ask1_size;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// merges partial ticker frames; a field never seen stays undef
struct FundingMemo {
    FixedE9 rate{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::bybit
