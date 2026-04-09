#pragma once

// What the public feed says a market did: its prints, its funding, and its
// 24-hour turnover.
//
// TWO CLOCKS ON ONE FRAME. Every stream timestamp — E and T — is in
// MICROseconds, while the next funding time on the same markPrice frame (`n`)
// is in MILLIseconds, as is every timestamp on the REST side. Read on the live
// feed 2026-09-24: E 1790280896846962 beside n 1790283600000.

#include <raikiri/venues/backpack/envelope.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::backpack {

// trade.<symbol>
struct WirePrint {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_id{0};
    std::uint64_t engine_us{0};
    bool buyer_is_maker{false}; // m: true => the aggressor sold
};

[[nodiscard]] std::expected<WirePrint, WireError> parse_trade(std::string_view data);

// markPrice.<symbol>. On a perpetual it carries the funding rate `f` for the
// interval that settles at `n`; on a spot pair the same stream answers with
// the mark alone, so an empty rate means "no funding", never "funding at zero".
struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ms{0};
    std::uint64_t event_us{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view data);

// One row of GET /api/v1/tickers — every market in one call, which is why the
// 24-hour window is pulled rather than streamed: a ticker stream would be one
// more key per instrument for a figure that barely moves in a minute.
struct WireTicker {
    std::string_view symbol;
    std::string_view volume;       // base asset
    std::string_view quote_volume; // USDC
};

inline constexpr std::size_t k_max_tickers = 512;

struct TickerBuffer {
    std::array<WireTicker, k_max_tickers> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError>
parse_tickers(std::string_view body, TickerBuffer& storage);

} // namespace raikiri::venues::backpack
