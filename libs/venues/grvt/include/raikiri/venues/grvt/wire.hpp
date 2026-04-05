#pragma once

// What GRVT sends, as views over the caller's buffer.
//
// EVERY NUMBER IS A STRING except two: `num_orders` on a level and
// `funding_interval_hours`. Prices and sizes are plain decimals — the SDK's
// "expressed in 9 decimals" describes an older wire and no longer holds — and
// times are unix NANOSECONDS, also as strings.

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::grvt {

using WireError = wire::Error;

// --- catalogue -----------------------------------------------------------

struct WireInstrument {
    std::string_view instrument; // BTC_USDT_Perp
    std::string_view base;
    std::string_view quote;
    std::string_view kind;   // PERPETUAL is the only one listed
    std::string_view status; // ACTIVE, DELISTED
    std::string_view tick_size;
    // The order size step as well as its floor: all 9866 levels of the
    // catalogue's books sat on it, checked 2026-09-24.
    std::string_view min_size;
    std::string_view min_notional;
    // Per contract, and 4 or 8: 60 of 184 settle every four hours.
    std::uint32_t funding_interval_hours{0};
    // In PERCENT per interval, like every funding figure this venue states.
    std::string_view funding_cap;
    std::string_view funding_floor;
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> rows{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

// {"code":1003,"message":"...","status":400}, on REST.
struct WireRestError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::optional<WireRestError> parse_rest_error(std::string_view body);

// --- the socket ----------------------------------------------------------

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0.0" deletes the level
};

// A snapshot runs to the whole book: 482 levels a side on BNB, the deepest
// in the catalogue on 2026-09-24.
inline constexpr std::size_t k_max_levels = 2048;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// v1.mini.d. A field the frame does not carry is UNCHANGED, and "" means the
// venue set it to null — so absent and empty are different facts here.
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

// v1.ticker.s. Funding figures are in PERCENT: "0.01" is one basis point.
struct WireTicker {
    std::string_view mark_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ns{0};
    std::uint32_t funding_interval_hours{0};
    // Taker buys plus taker sells is the whole turnover: every print has one
    // taker.
    std::string_view buy_volume_base;
    std::string_view sell_volume_base;
    std::string_view buy_volume_quote;
    std::string_view sell_volume_quote;
};

enum class FeedKind : std::uint8_t { Quote, Trade, Book, Ticker, Unknown };

// One feed frame. Only the payload `kind` names is filled.
struct WireFeed {
    FeedKind kind{FeedKind::Unknown};
    std::string_view stream;
    std::string_view selector; // BTC_USDT_Perp@500
    // 0 is a snapshot; a delta's `prev` names the frame it follows.
    std::uint64_t sequence{0};
    std::uint64_t prev_sequence{0};
    std::uint64_t event_ns{0};
    WireQuote quote;
    WireBook book;
    WirePrint print;
    WireTicker ticker;
};

inline constexpr std::size_t k_max_acked = 256;

// The JSON-RPC answer to a subscribe. `latest[i]` is what `subs[i]`'s
// snapshot describes, and the first delta after it names it as `prev`.
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

// The instrument half of a selector: BTC_USDT_Perp@500 -> BTC_USDT_Perp.
[[nodiscard]] std::string_view selector_instrument(std::string_view selector) noexcept;

} // namespace raikiri::venues::grvt
