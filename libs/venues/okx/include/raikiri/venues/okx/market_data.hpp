#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// side is the aggressor's side
struct WireTrade {
    std::string_view inst_id;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t seq_id{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// fundingTime is the settlement the current rate applies to; nextFundingTime is the one after
struct WireFundingRate {
    std::string_view inst_id;
    std::string_view funding_rate;
    std::string_view next_funding_rate; // forecast; observed empty in practice
    std::uint64_t funding_ms{0};
    std::uint64_t next_funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// on swap and futures vol24h counts contracts and volCcy24h is base;
// on spot and margin vol24h is base and volCcy24h quote
struct WireTicker {
    std::string_view inst_id;
    std::string_view inst_type;   // "SWAP" | "SPOT" | ...: decides the two below
    std::string_view vol_24h;     // vol24h
    std::string_view vol_ccy_24h; // volCcy24h
    std::string_view last;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// shared with GET /public/mark-price
struct WireMarkPrice {
    std::string_view inst_id;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// rate and mark arrive on separate channels and the shm cell is written whole, so both are merged
struct FundingMemo {
    FixedE9 rate{};
    FixedE9 predicted{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::okx
