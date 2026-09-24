#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::grvt {

using WireError = wire::Error;

struct WireInstrument {
    std::string_view instrument;
    std::string_view base;
    std::string_view quote;
    std::string_view kind;
    std::string_view status;
    std::string_view tick_size;
    // the size step as well as its floor
    std::string_view min_size;
    std::string_view min_notional;
    std::uint32_t funding_interval_hours{0};
    // percent per interval
    std::string_view funding_cap;
    std::string_view funding_floor;
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

struct WireRestError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::optional<WireRestError> parse_rest_error(std::string_view body);

// plain decimals, not the sdk's 9-decimal integers
struct WireLevel {
    std::string_view price;
    std::string_view size; // "0.0" deletes the level
};

inline constexpr std::size_t k_max_levels = 2048;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// absent means unchanged; "" means the venue nulled it
struct WireQuote {
    std::optional<std::string_view> bid_price;
    std::optional<std::string_view> bid_size;
    std::optional<std::string_view> ask_price;
    std::optional<std::string_view> ask_size;
};

struct WireBook {
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

struct WirePrint {
    std::string_view price;
    std::string_view size;
    bool taker_bought{false};
};

// funding figures are percent: "0.01" is one basis point
struct WireTicker {
    std::string_view mark_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ns{0};
    std::uint32_t funding_interval_hours{0};
    std::string_view buy_volume_base;
    std::string_view sell_volume_base;
    std::string_view buy_volume_quote;
    std::string_view sell_volume_quote;
};

enum class FeedKind : std::uint8_t { Quote, Trade, Book, Ticker, Unknown };

// only the payload kind names is filled
struct WireFeed {
    FeedKind kind{FeedKind::Unknown};
    std::string_view stream;
    std::string_view selector;
    // 0 on a snapshot.
    std::uint64_t sequence{0};
    std::uint64_t prev_sequence{0};
    std::uint64_t event_ns{0};
    WireQuote quote;
    WireBook book;
    WirePrint print;
    WireTicker ticker;
};

inline constexpr std::size_t k_max_acked = 256;

struct WireAck {
    std::string_view stream;
    std::span<const std::string_view> subs;
    std::span<const std::uint64_t> latest;
};

struct AckBuffer {
    std::array<std::string_view, k_max_acked> subs{};
    std::array<std::uint64_t, k_max_acked> latest{};
};

struct WireRpcError {
    std::int64_t code{0};
    std::string_view message;
};

enum class MessageKind : std::uint8_t { Feed, Ack, Error };

struct WireMessage {
    MessageKind kind{MessageKind::Feed};
    WireFeed feed;
    WireAck ack;
    WireRpcError error;
};

struct MessageBuffer {
    LevelBuffer levels;
    AckBuffer ack;
};

[[nodiscard]] std::expected<WireMessage, WireError> parse_message(std::string_view frame,
                                                                  MessageBuffer& storage);

[[nodiscard]] std::string_view selector_instrument(std::string_view selector) noexcept;

} // namespace raikiri::venues::grvt
