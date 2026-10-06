#pragma once

// market data only: orders are on-chain transactions or an allowlisted partner api

#include <cstdint>
#include <string_view>

namespace raikiri::venues::ostium::ep {

inline constexpr std::string_view k_host = "builder.prod.bedrock.ostium.io";

inline constexpr std::string_view k_pairs = "/v1/pairs";
inline constexpr std::string_view k_status = "/v1/status";
inline constexpr std::string_view k_subgraph = "/v1/subgraph/gn";
inline constexpr std::string_view k_ws_path = "/v1/prices/stream";

inline constexpr std::string_view k_rollover_query =
    R"({"query":"{ pairs(first: 1000) { id longOI shortOI lastRolloverLongPure brokerPremium isNegativeRolloverAllowed } }"})";

inline constexpr std::string_view k_fills_head =
    R"({"query":"{ orders(first: 1000, orderBy: executedAt, orderDirection: asc, where: {isPending: false, isCancelled: false, executedAt_gte: \")";
inline constexpr std::string_view k_fills_tail =
    R"(\"}) { id pair { id } orderAction isBuy priceAfterImpact tradeNotional closePercent executedAt } }"})";

inline constexpr std::uint64_t k_rollover_interval_ns = 30'000'000'000ULL;
inline constexpr std::uint64_t k_fills_interval_ns = 3'000'000'000ULL;
inline constexpr std::uint64_t k_window_s = 86'400;
// the indexer lands a fill some seconds after its block, and out of id order
inline constexpr std::uint64_t k_fills_overlap_s = 120;

} // namespace raikiri::venues::ostium::ep
