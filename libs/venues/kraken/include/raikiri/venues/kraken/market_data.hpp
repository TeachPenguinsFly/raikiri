#pragma once

// What the public feed says a market did: its prints, and the one ticker
// frame that carries top of book, mark, index and funding together.

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// One `ticker` frame carries top of book, mark, index and funding, which is
// why three subscription kinds resolve to this one feed.
//
// funding_rate and relative_funding_rate are NOT interchangeable. The
// absolute one changes units with the contract family, VERIFIED on one
// capture: PF_XBTUSD 0.9455796 / index 64108.28 = 1.4749e-5 ~= its
// relative 1.477885e-5, while PI_XBTUSD 1.39213091e-10 * index = 8.92e-6 ~=
// its relative 8.907125e-6 — reciprocal directions. Only the relative field
// is dimensionless and comparable across venues.
struct WireTicker {
    std::string_view product_id;
    std::string_view bid;
    std::string_view ask;
    std::string_view bid_size;
    std::string_view ask_size;
    std::string_view mark_price;
    std::string_view index;
    std::string_view last;
    std::string_view relative_funding_rate;            // dimensionless; use this
    std::string_view relative_funding_rate_prediction; // next interval forecast
    std::string_view open_interest;
    // The rolling 24-hour window, under Kraken's websocket spellings — the
    // REST ticker calls the first one `vol24h` and the two must not be
    // confused when reading a capture. `volume` counts the BASE asset only on
    // the PF_ family; on PI_ it counts CONTRACTS, each of which is a unit of
    // QUOTE, and Kraken then repeats it verbatim in volume_quote (VERIFIED
    // live: PI_XBTUSD 432254 in both, against PF_XBTUSD 12052.0267 and
    // 971669776.3224). map.cpp is where that split is applied.
    //
    // Empty when absent — ticker_lite carries neither — and empty is not zero.
    std::string_view volume;       // `volume`
    std::string_view volume_quote; // `volumeQuote`
    std::uint64_t next_funding_rate_time_ms{0};
    std::uint64_t time_ms{0};
    bool suspended{false};
};

// Reads both `ticker` and `ticker_lite`; the lite feed leaves the derivative
// fields absent, which is why they are optional here.
[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `side` is the AGGRESSOR's side — Kraken states the taker direction
// directly, so nothing is inverted here or in the mapper. `type` is "fill"
// for ordinary prints and names a forced event otherwise; a liquidation is
// real volume at a real price, so it is surfaced rather than filtered.
struct WireTrade {
    std::string_view product_id;
    std::string_view uid;
    std::string_view type;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t seq{0};
    std::uint64_t time_ms{0};
};

// A `trade_snapshot` replays the last 100 prints on subscribe.
inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// Reads `trade` (one print at the root) and `trade_snapshot` (a `trades`
// array) through one path, so the caller never branches on which it got.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

} // namespace raikiri::venues::kraken
