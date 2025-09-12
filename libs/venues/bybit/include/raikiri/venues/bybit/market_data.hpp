#pragma once

// What the public feed says a market did: its prints, and the ticker's running
// picture of it.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    bool buyer_aggressor{false}; // "S":"Buy" means the taker bought
    std::uint64_t ts_ms{0};
    std::string_view trade_id;
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    WireTrade trades[k_max_trades];
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// tickers.SYMBOL on linear perps pushes a full snapshot once and then
// DELTAS containing only changed fields — so every field here is optional
// and an absent one means "unchanged", not "zero". Getting this wrong
// silently zeroes a funding rate, which is why the flags exist.
struct WireTicker {
    std::string_view symbol;
    bool is_snapshot{false};
    std::string_view mark_price; // empty when absent
    std::string_view index_price;
    std::string_view funding_rate;
    std::string_view next_funding_ms;
    // EMPTY ON A DELTA, and empty is not zero: bybit sends the 24-hour
    // figures on the snapshot and omits them from every delta that did not
    // change them. Publishing a zero here would erase the real turnover on
    // the next tick.
    std::string_view volume_24h;   // base units
    std::string_view turnover_24h; // quote units
    std::string_view bid1_price;
    std::string_view bid1_size;
    std::string_view ask1_price;
    std::string_view ask1_size;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

} // namespace raikiri::venues::bybit
