#pragma once

// What the public channels say a market did: its top of book with the day's
// turnover, and its prints.

#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::coinbase {

// The channel also carries price/last_size/side for the match that triggered
// it; those are not read here because the matches channel prints the same
// trade with the side the mapper can actually use (see WireMatch).
struct WireTicker {
    std::string_view product_id;
    std::string_view best_bid;
    std::string_view best_bid_size;
    std::string_view best_ask;
    std::string_view best_ask_size;
    std::string_view time; // ISO-8601
    // The rolling 24-hour volume, in the BASE asset. This is the EXCHANGE
    // API's spelling; Advanced Trade calls the same figure `volume_24_h` on a
    // different host and this adapter does not speak to it.
    //
    // There is no quote-side column on this channel — `volume_30d` is another
    // window in the same base units, not a turnover — so the quote side of
    // TickerStats stays a hard zero. Every frame is a full snapshot, not a
    // delta, so an empty value here means a schema change, not "unchanged".
    std::string_view volume_24h;
    std::int64_t sequence{0};
    std::int64_t trade_id{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `side` on this channel is the MAKER's side, so the aggressor took the
// opposite one — hence the field name, which makes the flip in map.cpp
// impossible to read as an identity. VERIFIED live: 21 prints joined to their
// ticker frames by trade_id disagreed on `side` every single time, and every
// print carrying side "sell" landed on the ask, where a taker who bought
// would print. Binance's `m` needs the same care and is the field most often
// read backwards; okx's `side` is already the taker's and needs no flip.
struct WireMatch {
    std::string_view product_id;
    std::string_view price;
    std::string_view size;
    std::string_view time; // ISO-8601
    bool maker_sold{false};
    std::int64_t sequence{0};
    std::int64_t trade_id{0};
};

[[nodiscard]] std::expected<WireMatch, WireError> parse_match(std::string_view frame);

} // namespace raikiri::venues::coinbase
