#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::kucoin {

// side is already the aggressor's; binance's m means the opposite
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame, MarketType market);

[[nodiscard]] std::expected<WireTrade, WireError> parse_uta_trade(std::string_view frame);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// rest names the funding index in symbol; granularity differs between ws and rest, so it is ignored
struct WireFundingRate {
    std::string_view symbol;
    std::string_view rate;
    std::string_view predicted_rate; // REST `predictedValue`; absent in practice
    std::uint64_t granularity_ms{0};
    std::uint64_t next_funding_ms{0}; // REST `fundingTime`; 0 on the websocket
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// seeds funding at subscribe; the only source of the next settlement time
[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_current(std::string_view body);

// funding.rate and mark.index.price arrive separately and the shm cell is written whole, so both are kept and merged
struct FundingMemo {
    FixedE9 rate{};
    FixedE9 predicted{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::kucoin
