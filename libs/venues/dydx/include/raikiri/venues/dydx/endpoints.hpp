#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::dydx::ep {

inline constexpr std::string_view k_indexer_live = "indexer.dydx.trade";
inline constexpr std::string_view k_indexer_test = "indexer.v4testnet.dydx.exchange";

[[nodiscard]] constexpr std::string_view indexer_host(bool testnet) noexcept {
    return testnet ? k_indexer_test : k_indexer_live;
}

inline constexpr std::string_view k_ws = "/v4/ws";
inline constexpr std::string_view k_perpetual_markets = "/v4/perpetualMarkets";

// Per IP, in a fixed window; past it the indexer answers 429 with retry-after 10.
inline constexpr std::uint32_t k_rest_requests_per_window = 100;
inline constexpr std::uint64_t k_rest_window_ns = 10ULL * 1'000'000'000ULL;

// Per channel and ticker; one more within the second closes the socket.
inline constexpr std::uint32_t k_subscribes_per_key_per_s = 2;

} // namespace raikiri::venues::dydx::ep
