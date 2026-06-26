#pragma once

// Market data and funding only: an order is an Ed25519-signed request, which no
// signer here produces.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::pacifica::ep {

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? "test-api.pacifica.fi" : "api.pacifica.fi";
}

[[nodiscard]] constexpr std::string_view ws_host(bool testnet) noexcept {
    return testnet ? "test-ws.pacifica.fi" : "ws.pacifica.fi";
}

inline constexpr std::string_view k_markets = "/api/v1/info";
inline constexpr std::string_view k_ws_path = "/ws";

// The stated default is 20 subscriptions per channel per connection; holding
// the whole socket to 20 keeps any mix of channels under it.
inline constexpr std::size_t k_max_keys_per_connection = 20;

// 100 connections per IP, stated.
inline constexpr std::size_t k_max_connections = 24;

inline constexpr std::uint64_t k_subscribe_spacing_ns = 100ULL * 1'000'000ULL;

// A socket that has sent nothing for 60 s is closed; inbound data does not count.
inline constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

inline constexpr std::string_view k_ping_frame = R"({"method":"ping"})";

} // namespace raikiri::venues::pacifica::ep
