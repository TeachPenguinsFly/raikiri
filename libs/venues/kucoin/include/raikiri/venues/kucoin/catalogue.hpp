#pragma once

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// base units per contract; inverse rows state -1 and are refused
struct WireContract {
    std::string_view symbol;
    std::string_view base_currency; // "XBT" where the symbol says XBTUSDTM
    std::string_view quote_currency;
    std::string_view status; // "Open"
    std::string_view type;   // FFWCSX perpetual, FFICSX dated
    bool is_inverse{false};
    std::string_view asset_class; // CRYPTO, STOCK, COMMODITY, METAL
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view multiplier;
    std::string_view index_tick_size;
    std::string_view max_order_qty;
    // kucoin states 24h turnover only on the catalogue row
    std::string_view volume_24h;   // volumeOf24h, base asset
    std::string_view turnover_24h; // turnoverOf24h, quote currency
    std::uint64_t funding_granularity_ms{0};
    // the only funding source on the uta feed; empty when the contract states none
    std::string_view funding_fee_rate;           // fundingFeeRate
    std::string_view predicted_funding_fee_rate; // predictedFundingFeeRate, null in practice
    std::uint64_t next_funding_ms{0};            // nextFundingRateDateTime
};

inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// base_increment is the lot step, price_increment the tick, min_funds a quote-currency floor
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view base_increment;
    std::string_view price_increment;
    std::string_view min_funds;
    bool enable_trading{false};
};

inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

// the only place spot states its 24h window; vol is base, volValue quote
struct WireSpotTicker {
    std::string_view symbol;
    std::string_view vol;
    std::string_view vol_value;
    std::uint64_t ts_ms{0};
};

struct SpotTickerBuffer {
    std::array<WireSpotTicker, k_max_spot_symbols> tickers{};
};

[[nodiscard]] std::expected<std::span<const WireSpotTicker>, WireError>
parse_spot_tickers(std::string_view body, SpotTickerBuffer& storage);

} // namespace raikiri::venues::kucoin
