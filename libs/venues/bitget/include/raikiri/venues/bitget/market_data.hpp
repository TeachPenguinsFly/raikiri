#pragma once

// What the public feed says a market did: its prints, the ticker that carries
// top of book, mark and funding on one frame, and the funding endpoint that
// spells every one of those fields differently.

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// One `ticker` element. index_price is carried but unmapped: nothing above
// consumes an index yet, and dropping it would hide that Bitget sends it.
struct WireTicker {
    std::string_view inst_id;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::string_view last_price;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    // Bitget's own names, and the pair is not interchangeable: `baseVolume`
    // counts the BASE asset, `quoteVolume` the same window in the QUOTE. The
    // REST row carries a third, `usdtVolume`, which equals quoteVolume only on
    // a USDT-margined contract and is deliberately not read.
    //
    // EMPTY IS NOT ZERO. Every observed frame on this channel is a full
    // snapshot (MEASURED 2026-09-04: 317 of 317 carried action "snapshot" and
    // both columns), but a frame that omitted one must leave it empty rather
    // than publish a zero, which would erase a day's turnover from the board.
    std::string_view volume_24h_base;  // baseVolume
    std::string_view volume_24h_quote; // quoteVolume
    std::uint64_t next_funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `side` is already the AGGRESSOR's side — a print at the bid reads "sell",
// confirmed against a live book — so nothing is inverted here or in the
// mapper. Binance's `m` says the opposite and needs a flip; reading the two
// the same way is how a signed flow series comes out backwards.
struct WireTrade {
    std::string_view inst_id; // from the frame's arg: the element carries none
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// /api/v2/mix/market/current-fund-rate. Its field names match nothing else
// Bitget publishes: `fundingRateInterval` in hours, and `nextUpdate` for the
// instant the ticker channel calls `nextFundingTime`.
struct WireFundingRate {
    std::string_view symbol;
    std::string_view funding_rate;
    std::string_view interval_hours;
    std::string_view min_rate;
    std::string_view max_rate;
    std::uint64_t next_update_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError>
parse_current_fund_rate(std::string_view body);

} // namespace raikiri::venues::bitget
