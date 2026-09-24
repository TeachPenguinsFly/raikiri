#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::dydx {

using WireError = wire::Error;

struct WireMarket {
    std::string_view ticker; // BTC-USD
    std::string_view status; // ACTIVE, FINAL_SETTLEMENT, ...
    std::string_view tick_size;
    std::string_view step_size;
    std::int64_t atomic_resolution{0};
    std::int64_t quantum_conversion_exponent{0};
    std::uint64_t step_base_quantums{0};
    std::uint64_t subticks_per_tick{0};
    std::string_view oracle_price;
    std::string_view next_funding_rate; // per hour
    std::string_view volume_24h;        // in USD, not base
};

inline constexpr std::size_t k_max_markets = 1024;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError>
parse_perpetual_markets(std::string_view body, MarketBuffer& storage);

enum class FrameType : std::uint8_t { Connected, Subscribed, Unsubscribed, Data, Error, Unknown };

enum class Channel : std::uint8_t { Book, Trades, Other };

inline constexpr std::string_view k_book_channel = "v4_orderbook";
inline constexpr std::string_view k_trades_channel = "v4_trades";

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0" deletes the level
};

struct WirePrint {
    std::string_view price;
    std::string_view size;
    std::string_view side; // the taker's
    std::string_view created_at;
};

inline constexpr std::size_t k_max_levels = 8192;
inline constexpr std::size_t k_max_prints = 256;

struct FrameBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
    std::array<WirePrint, k_max_prints> prints{};
};

// A batch concatenates per side in arrival order, which is all a ladder needs:
// the two sides never touch each other's levels.
struct WireFrame {
    FrameType type{FrameType::Unknown};
    Channel channel{Channel::Other};
    std::string_view id; // the ticker; absent on most refusals
    std::string_view message;
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
    std::span<const WirePrint> prints;
    // Past the buffer: the levels read are a prefix, and a book must not take them.
    bool overflow{false};
};

// The trades backlog on `subscribed` is not read.
[[nodiscard]] std::expected<WireFrame, WireError> parse_frame(std::string_view frame,
                                                              FrameBuffer& storage);

// The channel a refusal names in its text, e.g. "... limit reached for v4_orderbook (limit=32).".
[[nodiscard]] Channel refused_channel(std::string_view message) noexcept;

[[nodiscard]] std::string_view channel_name(Channel channel) noexcept;

class Protocol {
public:
    [[nodiscard]] std::string_view subscribe(Channel channel, std::string_view ticker);
    [[nodiscard]] std::string_view unsubscribe(Channel channel, std::string_view ticker);

private:
    std::array<char, 192> frame_{};
};

} // namespace raikiri::venues::dydx
