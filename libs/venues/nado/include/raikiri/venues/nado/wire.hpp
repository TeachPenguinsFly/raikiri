#pragma once

// Market data only: an order is an eip-712 signature over the ink clearinghouse, which no signer here produces.
// Prices, sizes and rates are integers scaled by 1e18; stamps are nanoseconds unless named otherwise.

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::nado {

using WireError = wire::Error;

namespace ep {

inline constexpr std::string_view k_gateway = "gateway.prod.nado.xyz";
inline constexpr std::string_view k_archive = "archive.prod.nado.xyz";

inline constexpr std::string_view k_symbols = "/v1/query?type=symbols";
// a plain millisecond count
inline constexpr std::string_view k_time = "/v1/time";
// prefix; the product id and depth follow
inline constexpr std::string_view k_liquidity = "/v1/query?type=market_liquidity&depth=100&product_id=";
// every perp's mark, index, open interest and 24h window in one call
inline constexpr std::string_view k_contracts = "/v2/contracts";
inline constexpr std::string_view k_ws_path = "/v1/subscribe";

// the gateway answers 403 unless Accept-Encoding names gzip, br or deflate; deflate at q=0 passes and keeps the body
// plain
inline constexpr std::string_view k_accept_encoding = "deflate;q=0";

} // namespace ep

struct WireSymbol {
    std::string_view symbol; // BTC-PERP
    std::string_view type;   // perp, spot
    std::string_view status; // live, post_only, soft_reduce_only, not_tradable
    std::string_view tick_x18;
    std::string_view lot_x18;
    std::string_view min_notional_x18;
    std::string_view maker_fee_x18;
    std::string_view taker_fee_x18;
    std::uint32_t product_id{0};
};

inline constexpr std::size_t k_max_symbols = 512;

struct SymbolBuffer {
    std::array<WireSymbol, k_max_symbols> rows{};
};

// data.symbols is an object keyed by symbol
[[nodiscard]] std::expected<std::span<const WireSymbol>, WireError> parse_symbols(std::string_view body,
                                                                                SymbolBuffer& storage);

enum class EventKind : std::uint8_t { Unknown = 0, Bbo, Trade, Depth, Funding, Reply };

// classifies by `type`; a subscribe answer is {"result":null,"id":n} and carries `error` when refused
struct EventInfo {
    EventKind kind{EventKind::Unknown};
    bool refused{false};
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

struct WireBbo {
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t ts_ns{0};
    std::uint32_t product_id{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// no trade id; taker_qty and maker_qty are the same fill seen from either side
struct WireTrade {
    std::string_view price;
    std::string_view qty;
    std::uint64_t ts_ns{0};
    std::uint32_t product_id{0};
    bool taker_bought{false};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty; // absolute; zero removes the level
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// a book_depth push chains on last_max_timestamp == the previous push's max_timestamp; there is no sequence.
// the market_liquidity answer is a whole book stamped by `timestamp`, read into max_ts
struct WireDepth {
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
    std::uint64_t last_max_ts{0};
    std::uint64_t max_ts{0};
    std::uint32_t product_id{0};
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage);

[[nodiscard]] std::expected<WireDepth, WireError> parse_liquidity(std::string_view body, LevelBuffer& storage);

// funding_rate_x18 is the 24h rate; settlement is hourly at a 24th of it. update_time is seconds
struct WireFunding {
    std::string_view rate_x18;
    std::uint64_t ts_ns{0};
    std::uint64_t update_s{0};
    std::uint32_t product_id{0};
};

[[nodiscard]] std::expected<WireFunding, WireError> parse_funding(std::string_view frame);

// archive /v2/contracts: decimals as json numbers, keyed by ticker_id
struct WireContract {
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view open_interest; // base
    std::string_view base_volume;
    std::string_view quote_volume;
    std::uint32_t product_id{0};
};

struct ContractBuffer {
    std::array<WireContract, k_max_symbols> rows{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                                    ContractBuffer& storage);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time_ms(std::string_view body);

} // namespace raikiri::venues::nado
