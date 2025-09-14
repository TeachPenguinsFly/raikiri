#pragma once

// What the public feed says a market did: its prints, the funding it quotes,
// and the 24-hour window it states.
//
// NEITHER FUNDING NOR THE 24-HOUR WINDOW ARRIVES ON A SOCKET HERE. The
// markPriceUpdate stream is acknowledged on this venue and then never sends a
// frame, so both are REST polls driven from data_client — which is why the
// stream shape and the poll shape sit side by side in this file.

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// One public print. Named for what it is rather than for the channel, because
// the same shape arrives on @trade and on @aggTrade and the adapter reads
// either; the account's own fill is WireTrade, in orders.hpp.
struct WirePrint {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_id{0}; // aggregate id or trade id, whichever arrived
    std::uint64_t trade_ms{0};
    bool buyer_is_maker{false}; // m: true => the aggressor sold
};

[[nodiscard]] std::expected<WirePrint, WireError> parse_trade(std::string_view data);

// markPriceUpdate carries mark, index, funding rate and the next funding
// time together — one subscription covers what bybit splits across fields of
// its tickers stream.
struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ms{0};
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view data);

// GET /fapi/v1/premiumIndex, which carries the same facts as the markPriceUpdate
// stream under different names — lastFundingRate for r, nextFundingTime for T.
// It exists because that stream is acknowledged and then never sends a frame on
// this venue, so the rate has to be pulled or it does not arrive at all.
inline constexpr std::size_t k_max_premium_rows = 2048;

struct PremiumBuffer {
    std::array<WireMarkPrice, k_max_premium_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarkPrice>, WireError>
parse_premium_index(std::string_view body, PremiumBuffer& storage);

// GET /fapi/v1/ticker/24hr. The 24-hour window binance states per symbol; the
// mark-price stream this adapter polls carries no turnover at all, so this is
// a second read rather than another field on the first.
struct WireTicker24h {
    std::string_view symbol;
    std::string_view volume;       // base asset
    std::string_view quote_volume; // quote currency
    std::uint64_t close_ms{0};
};

inline constexpr std::size_t k_max_ticker_rows = 2048;

struct Ticker24hBuffer {
    std::array<WireTicker24h, k_max_ticker_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker24h>, WireError>
parse_ticker_24h(std::string_view body, Ticker24hBuffer& storage);

} // namespace raikiri::venues::binance
