#pragma once

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// bid1/ask1 carry no sizes, so the bbo comes from depth.full; fair_price is the mark
struct WireTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view ask_price;
    std::string_view last_price;
    std::string_view fair_price;
    std::string_view index_price;
    std::string_view funding_rate;
    // `amount24` is quote turnover; `volume24` counts contracts
    std::string_view volume_24h_quote;     // `amount24`
    std::string_view volume_24h_contracts; // `volume24`
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `T` is the taker direction, 1 buy 2 sell; no flip
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty; // contracts, not base units
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

// the only source of the settlement schedule; collect_cycle_h is in hours
struct WireFundingRate {
    std::string_view symbol;
    std::string_view funding_rate;
    std::string_view fair_price;
    std::uint64_t next_settle_ms{0};
    std::int64_t collect_cycle_h{0};
    std::uint64_t ts_ms{0};
    // per-interval cap and floor as fractions; may be absent, read as undef
    std::string_view max_rate;
    std::string_view min_rate;
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body);

// the only place mexc states a settlement interval; contracts settle every 1, 4, 8 or 24 h
struct FundingRateBuffer {
    std::array<WireFundingRate, 2048> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFundingRate>, WireError>
parse_funding_rates(std::string_view body, FundingRateBuffer& storage);

// one print: #1 price, #2 qty, #3 tradeType (1 is a taker buy), #4 time ms, #5 id
struct StreamTrade {
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

struct StreamTradeBuffer {
    std::array<StreamTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const StreamTrade>, WireError> parse_stream_trades(std::string_view payload,
                                                                                         StreamTradeBuffer& storage);

// every spot pair's 24h window in one answer
struct WireSpotTicker {
    std::string_view symbol;
    std::string_view volume_base;  // `volume`
    std::string_view volume_quote; // `quoteVolume`
    std::uint64_t close_ms{0};
};

struct SpotTickerBuffer {
    std::array<WireSpotTicker, 4096> rows{};
};

[[nodiscard]] std::expected<std::span<const WireSpotTicker>, WireError> parse_spot_tickers(std::string_view body,
                                                                                           SpotTickerBuffer& storage);

} // namespace raikiri::venues::mexc
