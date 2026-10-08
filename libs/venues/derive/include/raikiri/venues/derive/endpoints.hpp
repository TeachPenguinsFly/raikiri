#pragma once

// rest stays on v2; the v2 socket answers 503 since 2026-10-08 and v3 streams the same channels and frames

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::derive::ep {

inline constexpr std::string_view k_host = "api.lyra.finance";
inline constexpr std::string_view k_ws_host = "api.derive.xyz";
inline constexpr std::string_view k_ws_path = "/v3/ws";

inline constexpr std::string_view k_all_instruments = "/public/get_all_instruments";

// gatewayd's shm tables hold this many instruments; the furthest expiries are cut first
inline constexpr std::size_t k_catalogue_cap = 4096;

} // namespace raikiri::venues::derive::ep
