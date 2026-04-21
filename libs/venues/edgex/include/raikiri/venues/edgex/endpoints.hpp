#pragma once

#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::edgex::ep {

// V2 only. The V1 host (pro.edgex.exchange/api/v1) still answers "SUCCESS"
// with empty data on every quote, which looks healthy and is not.
inline constexpr std::string_view k_perp_rest = "edgex-prod-v2.edgex.exchange";
inline constexpr std::string_view k_perp_stream = "edgex-quote-prod-v2.edgex.exchange";
// Undocumented; the protocol is the perp socket's.
inline constexpr std::string_view k_spot_rest = "spot.edgex.exchange";
inline constexpr std::string_view k_spot_stream = "spot-quote.edgex.exchange";

[[nodiscard]] constexpr std::string_view rest_host(MarketType market) noexcept {
    return market == MarketType::Spot ? k_spot_rest : k_perp_rest;
}

[[nodiscard]] constexpr std::string_view stream_host(MarketType market) noexcept {
    return market == MarketType::Spot ? k_spot_stream : k_perp_stream;
}

inline constexpr std::string_view k_ws_path = "/api/v1/public/ws";

inline constexpr std::string_view k_perp_meta = "/api/v2/public/meta/getMetaData";
inline constexpr std::string_view k_spot_meta = "/api/v1/public/meta/getMetaData";

[[nodiscard]] constexpr std::string_view meta_path(MarketType market) noexcept {
    return market == MarketType::Spot ? k_spot_meta : k_perp_meta;
}

// Measured floors, not the venue's limits: no 429 at either, and the venue
// states none. The adapter makes one REST call per start.
inline constexpr std::uint32_t k_rest_rps_without_refusal = 135;
inline constexpr std::size_t k_subscriptions_without_refusal = 3601;

} // namespace raikiri::venues::edgex::ep
