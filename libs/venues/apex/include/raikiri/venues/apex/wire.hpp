#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::apex {

using WireError = wire::Error;

struct WireContract {
    // "BTCUSDT", the spelling every stream accepts; `symbol` ("BTC-USDT") is not.
    std::string_view cross_symbol;
    std::string_view base;
    std::string_view settle;
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view funding_max_rate; // per hour
    std::string_view funding_min_rate;
    bool enable_trade{false};
};

inline constexpr std::size_t k_max_contracts = 1024;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

// perpetualContract then stockContract. predictionContract is never read.
[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_symbols(std::string_view body, ContractBuffer& storage);

enum class TopicKind : std::uint8_t {
    Orderbook,  // orderBook25.H.{symbol}
    Trade,      // recentlyTrade.H.{symbol}
    Instrument, // instrumentInfo.H.{symbol}
    Ping,       // the server's {"op":"ping"}, which must be echoed
    Ack,        // {"success":..} answering a subscribe, unsubscribe or ping
    Unknown,
};

struct TopicInfo {
    TopicKind kind{TopicKind::Unknown};
    std::string_view symbol;
};

[[nodiscard]] TopicInfo classify_topic(std::string_view topic) noexcept;

[[nodiscard]] std::expected<TopicInfo, WireError> parse_topic(std::string_view frame);

inline constexpr std::size_t k_max_ack_args = 16;

struct WireAck {
    bool success{false};
    std::string_view ret_msg;
    std::string_view op;
    std::span<const std::string_view> args; // the request being answered, verbatim
};

struct AckBuffer {
    std::array<std::string_view, k_max_ack_args> args{};
};

[[nodiscard]] std::expected<WireAck, WireError> parse_ack(std::string_view frame,
                                                          AckBuffer& storage);

// The server's ping value, which the pong must carry back.
[[nodiscard]] std::expected<std::string_view, WireError> parse_ping(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0" deletes the level
};

struct WireOrderbook {
    std::string_view symbol;
    bool is_snapshot{false};
    std::uint64_t update_id{0};
    std::uint64_t ts_us{0}; // MICROseconds
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

inline constexpr std::size_t k_max_levels = 256;

struct OrderbookBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

[[nodiscard]] std::expected<WireOrderbook, WireError> parse_orderbook(std::string_view frame,
                                                                      OrderbookBuffer& storage);

struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    bool buyer_aggressor{false}; // "S":"Buy" is the taker buying
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

struct WireTrades {
    bool is_snapshot{false};
    std::span<const WireTrade> trades;
};

[[nodiscard]] std::expected<WireTrades, WireError> parse_trades(std::string_view frame,
                                                                TradeBuffer& storage);

// A delta carries only the fields that changed: empty means unchanged, never zero.
struct WireTicker {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view funding_rate;
    std::string_view next_funding_time; // ISO-8601, "2026-09-25T06:00:00Z"
    std::string_view volume_24h;        // base
    std::string_view turnover_24h;      // quote
    std::uint64_t ts_us{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

} // namespace raikiri::venues::apex
