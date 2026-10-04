#pragma once

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// qty is the magnitude of gate's signed size; the sign is aggressor_bought
struct WireTrade {
    std::string_view contract;
    std::string_view qty;
    std::string_view price;
    std::int64_t trade_id{0};
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// one frame can carry several prints
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage, MarketType market);

// funding and mark price on one channel; funding_rate_indicative is deprecated and not mapped
struct WireTicker {
    std::string_view contract;
    std::string_view funding_rate;
    std::string_view funding_rate_indicative;
    std::string_view mark_price;
    std::string_view index_price;
    // volume_24h alone is in contracts
    std::string_view volume_24h_base;
    std::string_view volume_24h_quote;
    std::uint64_t funding_next_apply_s{0};
    std::uint32_t funding_interval_s{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// options.contract_tickers: one contract per push, a full snapshot each second, stamped in whole seconds;
// vols are fractions, vega per vol point and theta per day, per unit of the underlying
struct WireOptionTicker {
    std::string_view contract;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view underlying_price;
    std::string_view bid_price;
    std::string_view ask_price;
    std::string_view bid_qty;
    std::string_view ask_qty;
    std::string_view mark_iv;
    std::string_view bid_iv;
    std::string_view ask_iv;
    std::string_view delta;
    std::string_view gamma;
    std::string_view vega;
    std::string_view theta;
    std::string_view rho;
    std::uint64_t ts_s{0};
};

[[nodiscard]] std::expected<WireOptionTicker, WireError> parse_option_ticker(std::string_view frame);

} // namespace raikiri::venues::gate
