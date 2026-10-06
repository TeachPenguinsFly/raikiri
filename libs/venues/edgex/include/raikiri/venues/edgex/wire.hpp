#pragma once

#include <raikiri/exec/data_client.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::edgex {

using WireError = wire::Error;

struct WireContract {
    std::uint32_t id{0};
    std::string_view name; // "BTCUSDC", "哈基米USDC", "ETH/USDC"
    std::string_view base; // the coin list's name for it: "HAJIMI" for 哈基米USDC
    std::string_view quote;
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_order_value; // spot only
    bool enabled{false};
    std::uint32_t funding_interval_min{0}; // perps; 0 when the row does not state it
    std::string_view funding_max;
    std::string_view funding_min;
};

inline constexpr std::size_t k_max_contracts = 1024;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> rows{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError> parse_perp_catalogue(std::string_view body,
                                                                                           ContractBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireContract>, WireError> parse_spot_catalogue(std::string_view body,
                                                                                           ContractBuffer& storage);

enum class EventKind : std::uint8_t {
    Depth,
    Trades,
    Ticker,
    Ping,
    Error,
    Other,
};

struct EventInfo {
    EventKind kind{EventKind::Other};
    bool snapshot{false};
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// the digits of {"type":"ping","time":"1790314760008"}, to be echoed
[[nodiscard]] std::expected<std::string_view, WireError> parse_ping(std::string_view frame);

struct StreamError {
    std::string_view code; // "GATEWAY_INVALID_CONTRACT_ID"
    std::string_view message;
    // the frame has no channel field; this comes from the echoed request
    std::string_view channel;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0" deletes the level
};

inline constexpr std::size_t k_max_levels = 256;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireDepth {
    std::uint32_t contract_id{0};
    bool snapshot{false};
    std::uint64_t start_version{0};
    std::uint64_t end_version{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage);

struct WirePrint {
    std::uint32_t contract_id{0};
    std::string_view price;
    std::string_view size;
    std::uint64_t ts_ms{0};
    bool buyer_maker{false};
};

inline constexpr std::size_t k_max_prints = 256;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

struct WireTicker {
    std::uint32_t contract_id{0};
    std::string_view mark_price;  // "0" on spot
    std::string_view index_price; // "0" on spot
    // the forecast for next_funding_ms; on fundingRate.* and rest the same name is the settled rate
    std::string_view funding_rate;
    std::uint64_t next_funding_ms{0};
    std::string_view volume_base;   // `size`
    std::string_view volume_quote;  // `value`
    std::string_view open_interest; // base; empty on spot
};

inline constexpr std::size_t k_max_tickers = 1024;

struct TickerBuffer {
    std::array<WireTicker, k_max_tickers> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view frame,
                                                                                  TickerBuffer& storage);

inline constexpr std::string_view k_ticker_all = "ticker.all.1s";
inline constexpr std::string_view k_depth_prefix = "depth.";
// 15 or 200; anything else is refused with GATEWAY_INVALID_BOOK_DEPTH.
inline constexpr std::uint32_t k_book_levels = 15;

// "depth.30000001.15" or "trades.30000001"; empty for a kind not read per
// contract.
[[nodiscard]] std::string_view
channel_name(exec::SubKind kind, std::uint32_t contract_id, std::span<char> out) noexcept;

// 30000001 out of "depth.30000001.15"; 0 when there is none.
[[nodiscard]] std::uint32_t depth_contract(std::string_view channel) noexcept;

enum class WsBuildError : std::uint8_t {
    BufferTooSmall,
};

class Protocol {
public:
    [[nodiscard]] std::expected<std::string_view, WsBuildError> subscribe_frame(std::string_view channel);

    [[nodiscard]] std::expected<std::string_view, WsBuildError> unsubscribe_frame(std::string_view channel);

    [[nodiscard]] std::expected<std::string_view, WsBuildError> pong_frame(std::string_view time);

private:
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    frame(std::string_view type, std::string_view key, std::string_view value);

    std::array<char, 128> frame_{};
};

} // namespace raikiri::venues::edgex
