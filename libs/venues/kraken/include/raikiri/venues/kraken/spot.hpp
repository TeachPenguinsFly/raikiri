#pragma once

// spot market data: the AssetPairs catalogue and the v2 socket's ticker, trade and book channels

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace raikiri::venues::kraken {

struct WireSpotPair {
    std::string_view altname;
    std::string_view wsname; // the v1 socket name, XBT/USD
    std::string_view status;
    std::string_view ordermin;
    std::string_view costmin;
    std::int64_t pair_decimals{-1};
    std::int64_t lot_decimals{-1};
};

inline constexpr std::size_t k_max_spot_pairs = 4096;

struct SpotPairBuffer {
    std::array<WireSpotPair, k_max_spot_pairs> pairs{};
};

// a non-empty error array refuses the whole body
[[nodiscard]] std::expected<std::span<const WireSpotPair>, WireError> parse_asset_pairs(std::string_view body,
                                                                                        SpotPairBuffer& storage);

// the v2 socket spells bitcoin BTC and dogecoin DOGE where wsname still has XBT and XDG
[[nodiscard]] std::optional<SymbolText> v2_symbol(std::string_view wsname) noexcept;

// online pairs only; tick and lot are 10^-decimals, which the book checksum relies on
[[nodiscard]] std::optional<Instrument> map_spot_pair(const WireSpotPair& wire);

enum class SpotChannel : std::uint8_t {
    Ticker,
    Trade,
    Book,
    Ack,     // a method answered with success
    Refused, // a method answered with success false
    Other,   // status, heartbeat, pong
};

struct SpotEvent {
    SpotChannel channel{SpotChannel::Other};
    bool snapshot{false};
};

[[nodiscard]] std::expected<SpotEvent, WireError> parse_spot_event(std::string_view frame);

struct WireSpotTicker {
    std::string_view symbol;
    std::string_view bid;
    std::string_view bid_qty;
    std::string_view ask;
    std::string_view ask_qty;
    std::string_view volume; // base, rolling 24 hours
    std::string_view vwap;   // over the same 24 hours
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireSpotTicker, WireError> parse_spot_ticker(std::string_view frame);

struct WireSpotTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t trade_id{0};
    std::uint64_t ts_ns{0};
};

// a snapshot replays 50 trades
inline constexpr std::size_t k_max_spot_trades = 128;

struct SpotTradeBuffer {
    std::array<WireSpotTrade, k_max_spot_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError> parse_spot_trades(std::string_view frame,
                                                                                         SpotTradeBuffer& storage);

[[nodiscard]] std::optional<Bbo>
map_spot_bbo(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_spot_ticker_stats(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_spot_trade(const WireSpotTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the checksum covers ten levels, so the book is held at ten
inline constexpr std::size_t k_spot_book_depth = 10;

[[nodiscard]] std::string_view spot_channel_for(exec::SubKind kind) noexcept;

// one subscribe or unsubscribe frame for one channel
[[nodiscard]] std::string
spot_frame(std::string_view method, std::string_view channel, std::span<const std::string_view> symbols);

} // namespace raikiri::venues::kraken
