#pragma once

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

// direction is the aggressor's side; amount is usd on reversed futures, coins on options
struct WireTrade {
    std::string_view instrument;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty; // `amount`
    bool aggressor_bought{false};
    std::int64_t trade_seq{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

// current_funding can be 0, so absence means a missing field; funding_8h is read but not mapped
struct WireTicker {
    std::string_view instrument;
    std::string_view mark_price;
    std::string_view current_funding; // empty on anything but a perpetual
    std::string_view funding_8h;      // empty on anything but a perpetual
    // options only; implied vols arrive as percents
    std::string_view delta;
    std::string_view gamma;
    std::string_view vega;
    std::string_view theta;
    std::string_view rho;
    std::string_view mark_iv;
    std::string_view bid_iv;
    std::string_view ask_iv;
    std::string_view underlying_price;
    std::string_view index_price;
    // volume is base units, volume_usd quote; a ticker without stats still yields funding
    std::string_view stats_volume;     // base units
    std::string_view stats_volume_usd; // quote units
    std::uint64_t ts_ms{0};
};

[[nodiscard]] constexpr bool has_greeks(const WireTicker& wire) noexcept {
    return !wire.delta.empty() || !wire.mark_iv.empty();
}

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

} // namespace raikiri::venues::deribit
