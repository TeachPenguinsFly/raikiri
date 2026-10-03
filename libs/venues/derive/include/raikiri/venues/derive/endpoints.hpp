#pragma once

// v2 carries the live books; v3 at api.derive.xyz/v3 lists the same channels

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::derive::ep {

inline constexpr std::string_view k_host = "api.lyra.finance";
inline constexpr std::string_view k_ws_path = "/ws";

inline constexpr std::string_view k_all_instruments = "/public/get_all_instruments";

// gatewayd's shm tables hold this many instruments; the furthest expiries are cut first
inline constexpr std::size_t k_catalogue_cap = 4096;

} // namespace raikiri::venues::derive::ep
