#pragma once

#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// order_size_min counts contracts; quanto_multiplier is base units per contract
struct WireContract {
    std::string_view name;
    std::string_view status; // prelaunch, trading, delisting, delisted, circuit_breaker
    std::string_view order_price_round;
    std::string_view order_size_min;
    std::string_view quanto_multiplier;
    std::string_view mark_price;
    std::string_view funding_rate;
    std::uint64_t funding_next_apply_s{0};
    std::uint32_t funding_interval_s{0};
    // symmetric: applied as +limit and -limit
    std::string_view funding_rate_limit;
    std::string_view contract_type; // "" crypto, stocks, indices, commodities, metals, forex
    bool in_delisting{false};
};

// past this a response is a venue fault or an allocation attack
inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

// the body is a top-level array
[[nodiscard]] std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                                      ContractBuffer& storage);

// precisions are digit counts: steps are 10^-n
struct WireCurrencyPair {
    std::string_view id;
    std::string_view trade_status; // tradable, untradable, buyable, sellable
    std::string_view min_base_amount;
    std::string_view min_quote_amount;
    std::uint32_t precision{0};
    std::uint32_t amount_precision{0};
};

inline constexpr std::size_t k_max_currency_pairs = 8192;

struct CurrencyPairBuffer {
    std::array<WireCurrencyPair, k_max_currency_pairs> pairs{};
};

[[nodiscard]] std::expected<std::span<const WireCurrencyPair>, WireError>
parse_currency_pairs(std::string_view body, CurrencyPairBuffer& storage);

} // namespace raikiri::venues::gate
