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

// sizes are whole contracts of `multiplier` underlying units; expiration_time is in seconds
struct WireOptionContract {
    std::string_view name; // BTC_USDT-20261225-90000-C
    std::string_view underlying;
    std::string_view strike_price;
    std::string_view order_price_round;
    std::string_view multiplier;
    std::uint64_t expiration_s{0};
    bool is_call{false};
    bool is_active{false};
};

inline constexpr std::size_t k_max_option_contracts = 2048;

struct OptionContractBuffer {
    std::array<WireOptionContract, k_max_option_contracts> contracts{};
};

// /options/contracts for one underlying; the body is a top-level array
[[nodiscard]] std::expected<std::span<const WireOptionContract>, WireError>
parse_option_contracts(std::string_view body, OptionContractBuffer& storage);

inline constexpr std::size_t k_max_option_underlyings = 64;

struct OptionUnderlyingBuffer {
    std::array<std::string_view, k_max_option_underlyings> names{};
};

// /options/underlyings: the names, BTC_USDT and the like
[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_option_underlyings(std::string_view body, OptionUnderlyingBuffer& storage);

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
