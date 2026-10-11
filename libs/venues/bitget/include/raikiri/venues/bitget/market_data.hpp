#pragma once

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// one row of the rest tickers list; spot rows carry no mark, index, funding or open interest
struct WireTicker {
    std::string_view inst_id; // symbol
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    // empty stays empty: a missing column must not publish a zero
    std::string_view volume_24h_base;  // baseVolume
    std::string_view volume_24h_quote; // quoteVolume
    std::string_view open_interest;    // holdingAmount, in the base coin
    std::uint64_t ts_ms{0};
};

// the spot list runs past 3 000 pairs
inline constexpr std::size_t k_max_tickers = 4096;

struct TickerBuffer {
    std::array<WireTicker, k_max_tickers> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

// side is already the aggressor's side; binance's m is the opposite
struct WireTrade {
    std::string_view inst_id; // from the frame's arg: the element carries none
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

// nextUpdate is the next settlement; fundingRateInterval is in hours
struct WireFundingRate {
    std::string_view symbol;
    std::string_view funding_rate;
    std::string_view interval_hours;
    std::string_view min_rate;
    std::string_view max_rate;
    std::uint64_t next_update_ms{0};
};

inline constexpr std::size_t k_max_fund_rates = 2048;

struct FundRateBuffer {
    std::array<WireFundingRate, k_max_fund_rates> rows{};
};

// one row with a symbol, every contract without
[[nodiscard]] std::expected<std::span<const WireFundingRate>, WireError>
parse_current_fund_rates(std::string_view body, FundRateBuffer& storage);

} // namespace raikiri::venues::bitget
