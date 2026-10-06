#pragma once

// market data only: orders need the eip-712 proxy signer, and access is not settled

#include <cstdint>
#include <string_view>

namespace raikiri::venues::polymarketperps::ep {

inline constexpr std::string_view k_rest_host = "api.perpetuals.polymarket.com";
inline constexpr std::string_view k_ws_host = "ws.perpetuals.polymarket.com";

inline constexpr std::string_view k_instruments = "/v1/info/instruments";
inline constexpr std::string_view k_bbo = "/v1/info/bbo";
inline constexpr std::string_view k_time = "/v1/info/time";
inline constexpr std::string_view k_ws_path = "/v1/ws";

inline constexpr std::string_view k_tickers = "tickers::all";
inline constexpr std::string_view k_statistics = "statistics::all";

// the venue closes a socket after 60 s of silence
inline constexpr std::uint64_t k_app_ping_interval_ns = 25'000'000'000ULL;
// tickers::all streams every instrument many times a second
inline constexpr std::uint64_t k_funding_spacing_ns = 1'000'000'000ULL;

inline constexpr std::string_view k_ping_frame = R"({"id":0,"req":"post","op":{"type":"ping"}})";

} // namespace raikiri::venues::polymarketperps::ep
