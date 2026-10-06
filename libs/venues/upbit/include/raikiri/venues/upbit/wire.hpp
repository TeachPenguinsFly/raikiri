#pragma once

// numbers arrive as json numbers, prices in exponent form as often as not; each is kept as its token

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::upbit {

using WireError = wire::Error;

inline constexpr std::size_t k_max_markets = 2048;

// "KRW-BTC": the quote comes first
struct WireMarket {
    std::string_view market;
};

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

struct WireTicker {
    std::string_view market;
    std::string_view trade_price;
    std::string_view volume_24h;   // base
    std::string_view turnover_24h; // quote
    std::uint64_t ts_ms{0};
};

struct TickerBuffer {
    std::array<WireTicker, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

// `{"error":{"name":...,"message":...}}`, on rest and on the socket; the name is a number or a word
struct WireRefusal {
    std::string_view name;
    std::string_view message;
};

[[nodiscard]] std::optional<WireRefusal> parse_refusal(std::string_view body);

enum class Kind : std::uint8_t { Unknown = 0, Orderbook, Trade, Refusal };

[[nodiscard]] Kind parse_kind(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 50;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// every frame is the whole top of the book; `ts` is in ms or µs, which differs by venue
struct WireBook {
    std::string_view code;
    std::span<const WireLevel> bids{};
    std::span<const WireLevel> asks{};
    std::uint64_t ts{0};
    bool snapshot{false};
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WireTrade {
    std::string_view code;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
    std::uint64_t sequential_id{0};
    bool snapshot{false};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame);

} // namespace raikiri::venues::upbit
