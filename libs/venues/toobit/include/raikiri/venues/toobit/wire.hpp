#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::toobit {

using WireError = wire::Error;

struct WireMarket {
    std::string_view symbol;
    std::string_view status;
    std::string_view base;
    std::string_view quote;
    std::string_view margin;
    std::string_view underlying;
    std::string_view tick;
    std::string_view step;
    std::string_view min_notional;
    std::string_view multiplier;
    bool contract{false};
    bool inverse{false};
};

inline constexpr std::size_t k_max_markets = 2048;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_exchange_info(std::string_view body,
                                                                                        MarketBuffer& storage);

struct WireFunding {
    std::string_view symbol;
    std::string_view rate;
    std::string_view period;
    std::string_view cap;
    std::string_view floor;
    std::uint64_t next_ms{0};
};

struct FundingBuffer {
    std::array<WireFunding, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFunding>, WireError> parse_funding_rates(std::string_view body,
                                                                                         FundingBuffer& storage);

struct WireTicker {
    std::string_view symbol;
    std::string_view volume;
    std::string_view quote_volume;
    std::uint64_t ts_ms{0};
};

struct TickerBuffer {
    std::array<WireTicker, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

enum class Channel : std::uint8_t { Unknown = 0, Bbo, Depth, Trade, Mark, Pong, Error };

// `first` marks the reply to a subscribe: an empty ack, a whole book or sixty old prints.
struct WireHead {
    Channel channel{Channel::Unknown};
    bool first{false};
    std::string_view symbol;
    std::string_view code;
    std::uint64_t send_ms{0};
};

[[nodiscard]] std::expected<WireHead, WireError> parse_head(std::string_view frame);

struct WireBbo {
    std::string_view symbol;
    std::string_view bid;
    std::string_view bid_qty;
    std::string_view ask;
    std::string_view ask_qty;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<std::optional<WireBbo>, WireError> parse_bbo(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireDepth {
    std::string_view symbol;
    bool first{false};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<std::optional<WireDepth>, WireError> parse_depth(std::string_view frame,
                                                                             LevelBuffer& storage);

struct WirePrint {
    std::uint64_t id{0};
    std::uint64_t ts_ms{0};
    std::string_view price;
    std::string_view qty;
    bool buyer_maker{false};
};

inline constexpr std::size_t k_max_prints = 256;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> rows{};
};

struct WirePrints {
    std::string_view symbol;
    bool first{false};
    std::span<const WirePrint> rows;
};

[[nodiscard]] std::expected<WirePrints, WireError> parse_trades(std::string_view frame, PrintBuffer& storage);

struct WireMark {
    std::string_view symbol;
    std::string_view price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<std::optional<WireMark>, WireError> parse_mark(std::string_view frame);

// a success body carries no code.
struct WireRestError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::optional<WireRestError> parse_rest_error(std::string_view body);

struct WireOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::string_view type;
    std::string_view side;
    std::string_view tif;
    std::string_view status;
    std::uint64_t time_ms{0};
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_rows = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_rows> rows{};
};

[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                                OrderBuffer& storage);

struct WirePosition {
    std::string_view symbol;
    std::string_view side;
    std::string_view avg_price;
    std::string_view position;
};

struct PositionBuffer {
    std::array<WirePosition, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

struct WireBalance {
    std::string_view coin;
    std::string_view balance;
    std::string_view available;
    std::string_view unrealized;
};

struct BalanceBuffer {
    std::array<WireBalance, 64> rows{};
};

[[nodiscard]] std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body,
                                                                                    BalanceBuffer& storage);

struct WireUserTrade {
    std::string_view id;
    std::string_view order_id;
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::string_view commission;
    std::string_view side;
    bool maker{false};
    std::uint64_t time_ms{0};
};

struct UserTradeBuffer {
    std::array<WireUserTrade, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError> parse_user_trades(std::string_view body,
                                                                                         UserTradeBuffer& storage);

[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

struct WireExecution {
    std::string_view symbol;
    std::string_view client_order_id;
    std::string_view order_id;
    std::string_view side;
    std::string_view order_type;
    std::string_view tif;
    std::string_view status;
    std::string_view qty;
    std::string_view price;
    std::string_view last_qty;
    std::string_view cum_qty;
    std::string_view last_price;
    std::string_view fee;
    bool maker{false};
    bool close{false};
    std::uint64_t event_ms{0};
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_executions = 64;

struct ExecutionBuffer {
    std::array<WireExecution, k_max_executions> rows{};
};

enum class PrivateKind : std::uint8_t { Unknown = 0, Events, ListenKeyExpiring, Pong };

struct WirePrivate {
    PrivateKind kind{PrivateKind::Unknown};
    std::span<const WireExecution> executions;
};

[[nodiscard]] std::expected<WirePrivate, WireError> parse_private(std::string_view frame, ExecutionBuffer& storage);

} // namespace raikiri::venues::toobit
