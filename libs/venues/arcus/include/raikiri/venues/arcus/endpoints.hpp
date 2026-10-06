#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::arcus::ep {

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? "api.testnet.arcus.xyz" : "api.arcus.xyz";
}

inline constexpr std::string_view k_markets = "/v1/markets";
inline constexpr std::string_view k_time = "/v1/time";
inline constexpr std::string_view k_ws_path = "/v1/ws";

// the snapshot's depth; deltas then run over the whole book
inline constexpr std::uint32_t k_book_depth = 100;

} // namespace raikiri::venues::arcus::ep
