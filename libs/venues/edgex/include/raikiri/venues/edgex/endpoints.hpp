#pragma once

#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::edgex::ep {

// v2 only; the v1 host answers SUCCESS with empty data
inline constexpr std::string_view k_perp_rest = "edgex-prod-v2.edgex.exchange";
inline constexpr std::string_view k_perp_stream = "edgex-quote-prod-v2.edgex.exchange";
// undocumented; the protocol is the perp socket's
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

inline constexpr std::uint32_t k_rest_rps_without_refusal = 135;

} // namespace raikiri::venues::edgex::ep
