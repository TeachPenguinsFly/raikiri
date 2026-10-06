#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// side is the aggressor's side
struct WireTrade {
    std::string_view inst_id;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t seq_id{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

// fundingTime is the settlement the current rate applies to; nextFundingTime is the one after
struct WireFundingRate {
    std::string_view inst_id;
    std::string_view funding_rate;
    std::string_view next_funding_rate; // forecast; observed empty in practice
    std::uint64_t funding_ms{0};
    std::uint64_t next_funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// on swap and futures vol24h counts contracts and volCcy24h is base;
// on spot and margin vol24h is base and volCcy24h quote
struct WireTicker {
    std::string_view inst_id;
    std::string_view inst_type;   // "SWAP" | "SPOT" | ...: decides the two below
    std::string_view vol_24h;     // vol24h
    std::string_view vol_ccy_24h; // volCcy24h
    std::string_view last;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// GET /market/tickers answers every instrument of one type in the same rows
inline constexpr std::size_t k_max_tickers = 4096;

struct TickerBuffer {
    std::array<WireTicker, k_max_tickers> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

// GET /public/open-interest: oi counts contracts, oiCcy is the coin on linear and inverse alike
struct WireOpenInterest {
    std::string_view inst_id;
    std::string_view oi_ccy;
    std::uint64_t ts_ms{0};
};

struct OpenInterestBuffer {
    std::array<WireOpenInterest, k_max_tickers> rows{};
};

[[nodiscard]] std::expected<std::span<const WireOpenInterest>, WireError>
parse_open_interest(std::string_view body, OpenInterestBuffer& storage);

// shared with GET /public/mark-price
struct WireMarkPrice {
    std::string_view inst_id;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// opt-summary, one row per option of the family; the BS greeks are in usd, the plain ones in the coin
struct WireOptSummary {
    std::string_view inst_id;
    std::string_view uly;
    std::string_view delta_bs;
    std::string_view gamma_bs;
    std::string_view vega_bs;
    std::string_view theta_bs;
    std::string_view mark_vol;
    std::string_view bid_vol;
    std::string_view ask_vol;
    std::string_view forward;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_summaries = 1024;

struct SummaryBuffer {
    std::array<WireOptSummary, k_max_summaries> rows{};
};

[[nodiscard]] std::expected<std::span<const WireOptSummary>, WireError> parse_opt_summary(std::string_view frame,
                                                                                          SummaryBuffer& storage);

struct WireIndexTicker {
    std::string_view inst_id;
    std::string_view index_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireIndexTicker, WireError> parse_index_ticker(std::string_view frame);

// an option's mark and its underlying's index ride channels apart from its greeks
struct OptionMemo {
    FixedE9 mark{};
    FixedE9 index{};
};

// rate and mark arrive on separate channels and the shm cell is written whole, so both are merged
struct FundingMemo {
    FixedE9 rate{};
    FixedE9 predicted{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::okx
