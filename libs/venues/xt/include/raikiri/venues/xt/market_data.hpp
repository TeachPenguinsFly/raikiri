#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::xt {

// m is the aggressor's side; a is contracts on futures, spot uses q in base units
struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame,
                                                              MarketType market);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// agg_ticker copies a into v, so only ticker is read; the base column is spot-only
struct WireTicker {
    std::string_view symbol;
    std::string_view volume_24h_base;  // spot `q`; always empty on futures
    std::string_view volume_24h_quote; // `v` on both trees
    std::string_view contracts_24h;    // futures `a`, in contracts
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame,
                                                                MarketType market);

// no funding channel exists, so this is polled from /public/q/funding-rate; interval_h (collectionInternal) is in hours
struct WireFundingRate {
    std::string_view symbol;
    std::string_view rate_token;
    std::uint64_t next_collection_ms{0};
    std::int64_t interval_h{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body);

// rate is polled and mark streamed but the shm record is written whole, so both are kept and merged
struct FundingMemo {
    FixedE9 rate{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::xt
