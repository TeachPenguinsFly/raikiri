#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::lbank {

using WireError = wire::Error;

// ticks arrive as json numbers
struct WirePerpInstrument {
    std::string_view symbol;
    std::string_view base;
    std::string_view quote;
    std::string_view price_tick;
    std::string_view volume_tick;
    std::string_view multiple;
    std::string_view min_cost;
    bool suspended{false};
};

inline constexpr std::size_t k_max_instruments = 2048;

struct PerpInstrumentBuffer {
    std::array<WirePerpInstrument, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePerpInstrument>, WireError>
parse_perp_instruments(std::string_view body, PerpInstrumentBuffer& storage);

struct WirePerpMarket {
    std::string_view symbol;
    std::uint32_t interval_s{0};
    std::uint64_t next_fee_ms{0};
};

struct PerpMarketBuffer {
    std::array<WirePerpMarket, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePerpMarket>, WireError> parse_perp_markets(std::string_view body,
                                                                                           PerpMarketBuffer& storage);

enum class Action : std::uint8_t { Unknown = 0, Book, Trade, Overview, Ack, Note };

struct WireHead {
    Action action{Action::Unknown};
    std::int64_t error_code{0};
};

[[nodiscard]] WireHead parse_head(std::string_view frame);

// rows at the instrument own tick carry no `Tick`; coarser groupings do
struct WireLevel {
    std::string_view instrument;
    std::string_view price;
    std::string_view volume;
    std::string_view tick;
    bool ask{false};
};

inline constexpr std::size_t k_max_levels = 4096;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> levels{};
};

// a snapshot has no `bNo`; a diff `bNo` is one counter shared by every instrument
struct WireBook {
    bool snapshot{false};
    std::string_view index;
    std::uint64_t bno{0};
    std::span<const WireLevel> levels;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WireTrade {
    std::string_view instrument;
    std::string_view trade_id;
    std::string_view price;
    std::string_view volume;
    std::uint64_t time_s{0};
    bool buy{false};
};

inline constexpr std::size_t k_max_trades = 256;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

struct WireOverview {
    std::string_view instrument;
    std::string_view mark;
    std::string_view fee_rate;
    std::string_view volume_24h;
    std::string_view turnover_24h;
    std::uint64_t update_s{0};
};

inline constexpr std::size_t k_max_overviews = 4096;

struct OverviewBuffer {
    std::array<WireOverview, k_max_overviews> rows{};
};

[[nodiscard]] std::expected<std::span<const WireOverview>, WireError> parse_overview(std::string_view frame,
                                                                                     OverviewBuffer& storage);

struct WireSpotPair {
    std::string_view symbol;
    std::string_view price_places;
    std::string_view qty_places;
    std::string_view min_qty;
    std::string_view min_amount;
};

struct SpotPairBuffer {
    std::array<WireSpotPair, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireSpotPair>, WireError> parse_spot_pairs(std::string_view body,
                                                                                       SpotPairBuffer& storage);

struct SymbolBuffer {
    std::array<std::string_view, k_max_instruments> symbols{};
};

[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_spot_ticker_symbols(std::string_view body, SymbolBuffer& storage);

enum class SpotChannel : std::uint8_t { Unknown = 0, Depth, Trade, Tick, Pong, Error };

[[nodiscard]] SpotChannel parse_spot_channel(std::string_view frame);

struct WirePair {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_spot_levels = 200;

struct SpotLevelBuffer {
    std::array<WirePair, k_max_spot_levels> bids{};
    std::array<WirePair, k_max_spot_levels> asks{};
};

// every depth frame is the whole top-n; `ds` is epoch milliseconds
struct WireSpotDepth {
    std::string_view pair;
    std::uint64_t ts_ms{0};
    std::span<const WirePair> bids;
    std::span<const WirePair> asks;
};

[[nodiscard]] std::expected<WireSpotDepth, WireError> parse_spot_depth(std::string_view frame,
                                                                       SpotLevelBuffer& storage);

struct WireSpotTrade {
    std::string_view pair;
    std::string_view price;
    std::string_view volume;
    std::uint64_t ts_ms{0};
    bool buy{false};
};

[[nodiscard]] std::expected<WireSpotTrade, WireError> parse_spot_trade(std::string_view frame);

struct WireSpotTick {
    std::string_view pair;
    std::string_view volume;
    std::string_view turnover;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireSpotTick, WireError> parse_spot_tick(std::string_view frame);

// beijing wall time with no zone, as epoch milliseconds; zero when malformed
[[nodiscard]] std::uint64_t beijing_to_ms(std::string_view text) noexcept;

} // namespace raikiri::venues::lbank
