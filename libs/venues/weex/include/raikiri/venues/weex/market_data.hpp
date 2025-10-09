#pragma once

// What the public feed says a market did: its prints, the 24-hour window it
// states, and the funding it quotes.
//
// FUNDING ARRIVES ON NO SOCKET HERE. `@fundingRate`, `@markPrice`,
// `@indexPrice`, `@premiumIndex`, `@bookTicker`, `@miniTicker` and `@aggTrade`
// were each subscribed alone on a fresh connection and each drew
// `INVALID_ARGUMENT: invalid event : <name>`; `@depth15`, `@depth200`,
// `@depth`, `@trade` and `@ticker` are the whole public list. So funding is a
// REST poll of /capi/v3/market/premiumIndex, which is why the stream shape and
// the poll shape sit side by side in this file.

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

// One public print off <sym>@trade.
//
// `t` IS A UUID, not a number: "83f2c4cd-2ddc-4c37-8a90-573e0d8dd7cf" live. A
// reader that took it as an integer would record every print under id 0. It is
// carried as text and published nowhere — it exists because the snapshot rule
// below is a statement about ids and a test has to be able to check it.
struct WirePrint {
    std::string_view trade_id;  // t
    std::string_view price;     // p
    std::string_view qty;       // q, in BASE units
    std::uint64_t trade_ms{0};  // T
    bool buyer_is_maker{false}; // m: true => the aggressor sold
};

inline constexpr std::size_t k_max_prints = 64;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

// The prints of one frame, in the order WEEX listed them.
//
// A `trade` frame is published; a `tradeSnapshot` frame is NOT, and the caller
// decides that from the event kind rather than from anything in here. The
// backfill is up to twenty historical prints and it OVERLAPS what follows:
// across two sessions five of its ids came round again on the first live
// frames, while the live stream repeated an id 0 times in 1457 prints. Nothing
// downstream deduplicates a trade, so publishing the backfill counts those
// prints twice in every signed-flow series that starts at a reconnect.
[[nodiscard]] std::expected<std::span<const WirePrint>, WireError>
parse_trades(std::string_view frame, PrintBuffer& storage);

// <sym>@ticker: the rolling 24-hour window, and the only place WEEX publishes
// turnover at all.
//
// WHAT THE FRAME DOES NOT CARRY decides half this adapter's shape. Its `d`
// object holds fourteen fields — p P w c o h l v q O C n m i — and among them
// there is NO bid, NO ask and NO funding rate. `m` is a mark price and `i` an
// index; neither is read. A mark with no rate beside it has nowhere to go in
// the sink, and the index failed to divide by its contract's own tick on 446 of
// 1022 rows, so it could only be carried by being rounded into a price the
// venue never quoted.
struct WireTicker {
    std::string_view symbol;
    std::string_view volume;       // v: base asset
    std::string_view quote_volume; // q: quote currency
    std::uint64_t close_ms{0};     // C: the window's close, not the send time
    std::uint64_t event_ms{0};     // E
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// One row of GET /capi/v3/market/premiumIndex, which answers for every contract
// at once — 1022 rows and 229 KiB measured — so nothing here asks per symbol.
//
// `collectCycle` is the funding period in MINUTES (480, 240 and 60 are the only
// values live), and it is NOT read for the interval: the catalogue states the
// same fact as a settlement timetable and the instrument already carries it.
// See catalogue.hpp for why the two agree.
struct WirePremium {
    std::string_view symbol;
    std::string_view mark_price;      // markPrice
    std::string_view funding_rate;    // lastFundingRate
    std::string_view forecast_rate;   // forecastFundingRate
    std::uint64_t next_funding_ms{0}; // nextFundingTime
    std::uint64_t event_ms{0};        // time
};

inline constexpr std::size_t k_max_premium_rows = 2048;

struct PremiumBuffer {
    std::array<WirePremium, k_max_premium_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePremium>, WireError>
parse_premium_index(std::string_view body, PremiumBuffer& storage);

} // namespace raikiri::venues::weex
