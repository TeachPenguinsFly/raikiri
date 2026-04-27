#pragma once

#include <raikiri/exec/data_client.hpp>
#include <raikiri/venues/phemex/map.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::phemex {

inline constexpr std::string_view k_real_book = "orderbook_p";
inline constexpr std::string_view k_scaled_book = "orderbook";
inline constexpr std::string_view k_real_trade = "trade_p";
inline constexpr std::string_view k_scaled_trade = "trade";

// One subscription each, carrying every symbol of its kind: asked once per
// socket pool, never once per instrument.
inline constexpr std::string_view k_perp_pack = "perp_market24h_pack_p";
inline constexpr std::string_view k_inverse_ticker = "market24h";
inline constexpr std::string_view k_spot_ticker = "spot_market24h";

// The venue refuses the 102nd subscription on a socket with 6015, across
// every channel together; the docs' 20 is not what it enforces.
inline constexpr std::size_t k_max_keys_per_connection = 90;
inline constexpr std::size_t k_max_connections = 8;
inline constexpr std::uint64_t k_subscribe_spacing_ns = 60ULL * 1'000'000ULL;

// Unpinged, the venue drops a socket at ~31 s with no close frame.
inline constexpr std::uint64_t k_ping_interval_ns = 10ULL * 1'000'000'000ULL;

// "orderbook_p:BTCUSDT", or the bare channel for the all-symbol feeds. Empty
// for a kind this venue does not stream.
[[nodiscard]] std::string_view channel_key(exec::SubKind kind,
                                           MarketType market,
                                           Encoding encoding,
                                           std::string_view symbol,
                                           std::span<char> out) noexcept;

// "orderbook_p:BTCUSDT" -> "BTCUSDT"; empty for a key naming no symbol.
[[nodiscard]] std::string_view key_symbol(std::string_view key) noexcept;

[[nodiscard]] bool is_book_key(std::string_view key) noexcept;

class Protocol {
public:
    // {"id":N,"method":"orderbook_p.subscribe","params":["BTCUSDT"]}; empty
    // when the key does not fit.
    [[nodiscard]] std::string_view subscribe_frame(std::string_view key);

    [[nodiscard]] static std::string_view ping_frame() noexcept;

private:
    std::array<char, 160> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::phemex
