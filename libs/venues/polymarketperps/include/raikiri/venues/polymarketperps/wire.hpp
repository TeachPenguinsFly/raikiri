#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::polymarketperps {

using WireError = wire::Error;

// one row of GET /v1/info/instruments
struct WireInstrument {
    std::uint64_t iid{0};
    std::string_view symbol;   // "BTC-USD"
    std::string_view base;     // "BTC", "GOLD", "SP500"
    std::string_view category; // crypto, equity, index, commodity
    std::string_view type;     // perpetual
    std::string_view min_notional;
    std::uint32_t price_decimals{0};
    std::uint32_t quantity_decimals{0};
    bool close_only{false};
};

inline constexpr std::size_t k_max_instruments = 512;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                                          InstrumentBuffer& storage);

enum class Frame : std::uint8_t { Unknown = 0, Bbo, Book, Trades, Ticker, Statistics, Answer };

[[nodiscard]] Frame parse_frame(std::string_view frame);

// refused channels in a subscribe answer
[[nodiscard]] std::size_t count_refusals(std::string_view frame);

// `sq` is the engine's one counter across every channel: it orders, it does not find gaps
struct WireBbo {
    std::uint64_t iid{0};
    std::uint64_t sq{0};
    std::uint64_t ts_ms{0};
    std::string_view bid;
    std::string_view bid_qty;
    std::string_view ask;
    std::string_view ask_qty;
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

struct BboBuffer {
    std::array<WireBbo, k_max_instruments> rows{};
};

// GET /v1/info/bbo, the seed for markets whose bbo channel has not moved
[[nodiscard]] std::expected<std::span<const WireBbo>, WireError> parse_bbo_rows(std::string_view body,
                                                                                BboBuffer& storage);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 1024;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// every book frame is the whole ladder
struct WireBook {
    std::uint64_t iid{0};
    std::uint64_t sq{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WirePrint {
    std::uint64_t iid{0};
    std::string_view side; // the taker's: long or short
    std::string_view price;
    std::string_view qty;
    std::uint64_t ts_ms{0};
    bool settlement{false};
};

inline constexpr std::size_t k_max_prints = 512;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

struct WireTicker {
    std::uint64_t iid{0};
    std::uint64_t ts_ms{0};
    std::string_view index;
    std::string_view mark;
    std::string_view open_interest; // base asset
    std::string_view funding;       // the running hourly rate
    std::uint64_t next_funding_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// the rolling 24 h volume in the quote currency
struct WireStatistics {
    std::uint64_t iid{0};
    std::uint64_t ts_ms{0};
    std::string_view volume;
};

[[nodiscard]] std::expected<WireStatistics, WireError> parse_statistics(std::string_view frame);

} // namespace raikiri::venues::polymarketperps
