#pragma once

#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::coinbase {

// price/last_size/side are skipped; the matches channel carries the same trade
struct WireTicker {
    std::string_view product_id;
    std::string_view best_bid;
    std::string_view best_bid_size;
    std::string_view best_ask;
    std::string_view best_ask_size;
    std::string_view time; // ISO-8601
    // rolling 24-hour volume in base; no quote column on this channel. frames are full, so empty means a schema change
    std::string_view volume_24h;
    std::int64_t sequence{0};
    std::int64_t trade_id{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `side` is the maker's; the aggressor took the other side
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
