#pragma once

// every number arrives as a decimal string; timestamps are json numbers in milliseconds

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::derive {

using WireError = wire::Error;

struct WireInstrument {
    std::string_view instrument_type; // option, perp, erc20
    std::string_view instrument_name;
    std::string_view base_currency;
    std::string_view quote_currency;
    std::string_view tick_size;
    std::string_view amount_step;
    std::string_view minimum_amount;
    std::string_view maker_fee_rate;
    std::string_view taker_fee_rate;
    std::string_view strike;      // options only
    std::string_view option_type; // C or P
    std::uint64_t expiry_s{0};
    std::uint64_t activation_s{0};
    bool is_active{false};
};

inline constexpr std::size_t k_page_size = 1000;

struct InstrumentBuffer {
    std::array<WireInstrument, k_page_size> rows{};
};

struct InstrumentPage {
    std::span<const WireInstrument> rows{};
    std::uint32_t num_pages{0};
};

[[nodiscard]] std::expected<InstrumentPage, WireError> parse_instruments(std::string_view body,
                                                                         InstrumentBuffer& storage);

// a json-rpc error object; nullopt when the body carries none
struct WireRpcError {
    std::int64_t code{0};
    std::string_view message{};
};

[[nodiscard]] std::optional<WireRpcError> parse_rpc_error(std::string_view body);

enum class Channel : std::uint8_t { Unknown = 0, Book, Ticker, Trades, Response };

struct Head {
    Channel channel{Channel::Unknown};
    std::string_view name{}; // the subscription channel, empty on a response
};

[[nodiscard]] Head parse_head(std::string_view frame);

// the instrument between the prefix and the last dotted parameters
[[nodiscard]] std::string_view instrument_of(std::string_view channel, std::size_t trailing_params) noexcept;

// refusals are counted per channel in the status map of a subscribe answer
struct WireSubscribeAck {
    std::uint32_t accepted{0};
    std::uint32_t refused{0};
};

[[nodiscard]] std::expected<WireSubscribeAck, WireError> parse_subscribe_ack(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 100;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// every frame is a whole snapshot of the subscribed depth
struct WireBook {
    std::string_view instrument;
    std::span<const WireLevel> bids{};
    std::span<const WireLevel> asks{};
    std::uint64_t publish_id{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

// ticker_slim; option_pricing is null off options and funding is null on them
struct WireTicker {
    std::string_view best_bid;
    std::string_view best_bid_qty;
    std::string_view best_ask;
    std::string_view best_ask_qty;
    std::string_view mark;
    std::string_view index;
    std::string_view funding; // per hour
    std::string_view contracts_24h;
    std::string_view notional_24h;
    std::string_view premium_24h;
    std::string_view open_interest; // contracts of one base unit
    bool priced{false};
    std::string_view delta;
    std::string_view gamma;
    std::string_view theta;
    std::string_view vega;
    std::string_view rho;
    std::string_view mark_iv;
    std::string_view bid_iv;
    std::string_view ask_iv;
    std::string_view forward;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// direction is the aggressor's side
struct WireTrade {
    std::string_view instrument;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

} // namespace raikiri::venues::derive
