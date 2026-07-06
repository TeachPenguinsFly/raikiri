#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill. Two hosts, two shapes: the futures
// contract row and the spot symbol row share nothing but the name of the
// symbol.

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// `multiplier` is how many base units one CONTRACT is worth, and it is negative
// on inverse contracts, whose size is quoted in the quote currency instead; the
// mapper refuses those rather than storing a negative size.
struct WireContract {
    std::string_view symbol;
    std::string_view base_currency; // "XBT" where the symbol says XBTUSDTM
    std::string_view quote_currency;
    std::string_view status; // "Open"
    std::string_view type;   // FFWCSX perpetual, FFICSX dated
    std::string_view asset_class; // CRYPTO, STOCK, COMMODITY, METAL
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view multiplier;
    std::string_view index_tick_size;
    std::string_view max_order_qty;
    // The 24-hour window rides the CATALOGUE row on this venue: kucoin
    // states no turnover on any stream the adapter subscribes to, and the
    // endpoint that lists the contracts states it per contract.
    std::string_view volume_24h;   // volumeOf24h, base asset
    std::string_view turnover_24h; // turnoverOf24h, quote currency
    std::uint64_t funding_granularity_ms{0};
    // FUNDING RIDES THIS ROW TOO, and on the unified feed it is the only
    // place it rides: UTA v2 carries books and nothing else, so a gateway on
    // it has no funding stream at all. Empty when the contract states none.
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

// GET /api/v2/symbols on the SPOT host. `base_increment` is the lot step and
// `price_increment` the tick; `min_funds` is a quote-currency floor, not a size.
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

// GET /api/v1/market/allTickers on the SPOT host: the only place spot states
// its 24-hour window. `vol` is the base asset, `volValue` the quote.
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
