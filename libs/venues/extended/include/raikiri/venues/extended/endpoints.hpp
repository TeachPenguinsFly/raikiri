#pragma once

// Market data and funding only: an order is a Stark-key signature over a
// settlement, which no signer here produces.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::extended::ep {

inline constexpr std::string_view k_host_live = "api.starknet.extended.exchange";
inline constexpr std::string_view k_host_test = "api.starknet.sepolia.extended.exchange";

[[nodiscard]] constexpr std::string_view host(bool testnet) noexcept {
    return testnet ? k_host_test : k_host_live;
}

inline constexpr std::string_view k_markets = "/api/v1/info/markets";

// A socket is one stream and, without a market in its path, carries every
// market: there are no subscribe frames.
enum class Stream : std::uint8_t { Book = 0, Top, Trades, Funding, Mark };

inline constexpr std::size_t k_stream_count = 5;

inline constexpr std::array<std::string_view, k_stream_count> k_stream_targets{
    "/stream.extended.exchange/v1/orderbooks",
    "/stream.extended.exchange/v1/orderbooks?depth=1",
    "/stream.extended.exchange/v1/publicTrades",
    "/stream.extended.exchange/v1/funding",
    "/stream.extended.exchange/v1/prices/mark",
};

[[nodiscard]] constexpr std::string_view target(Stream stream) noexcept {
    return k_stream_targets[static_cast<std::size_t>(stream)];
}

// 1000 a minute per IP, stated; declared at half because the window is fixed.
inline constexpr std::uint32_t k_rest_requests_per_minute = 500;

} // namespace raikiri::venues::extended::ep
