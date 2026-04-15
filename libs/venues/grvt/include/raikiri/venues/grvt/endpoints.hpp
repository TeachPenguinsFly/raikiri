#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::grvt::ep {

inline constexpr std::string_view k_host_live = "market-data.grvt.io";
inline constexpr std::string_view k_host_testnet = "market-data.testnet.grvt.io";

[[nodiscard]] constexpr std::string_view host(bool testnet) noexcept {
    return testnet ? k_host_testnet : k_host_live;
}

// full, not lite: lite's two-letter fields differ by a digit (lp last price, lp1 24h low).
inline constexpr std::string_view k_ws_path = "/ws/full";

inline constexpr std::string_view k_all_instruments = "/full/v1/all_instruments";
inline constexpr std::string_view k_all_instruments_body = R"({"is_active":true})";

inline constexpr std::uint32_t k_instrument_reads_per_minute = 1500;
inline constexpr std::uint32_t k_other_reads_per_minute = 500;

inline constexpr std::string_view k_quote_stream = "v1.mini.d";
inline constexpr std::string_view k_quote_rate = "@0";
inline constexpr std::string_view k_trade_stream = "v1.trade";
// 50 is the snapshot's length, not a rate; those prints are history and dropped.
inline constexpr std::string_view k_trade_rate = "@50";
// Deltas, not v1.book.s: the snapshot stream costs four times the bytes.
inline constexpr std::string_view k_book_stream = "v1.book.d";
inline constexpr std::string_view k_book_rate = "@500";
inline constexpr std::string_view k_ticker_stream = "v1.ticker.s";
inline constexpr std::string_view k_ticker_rate = "@5000";

// Below what the venue allows, so a dropped socket re-snapshots only part of the catalogue.
inline constexpr std::size_t k_max_keys_per_connection = 400;
inline constexpr std::size_t k_max_connections = 4;

inline constexpr std::size_t k_max_selectors_per_frame = 50;

// Stays under the stated 300 subscriptions a minute.
inline constexpr std::uint64_t k_subscribe_spacing_ns = 250ULL * 1'000'000ULL;

} // namespace raikiri::venues::grvt::ep
