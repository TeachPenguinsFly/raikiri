#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// From GET /futures/{settle}/contracts. `order_size_min` is a count of
// contracts, and `quanto_multiplier` is how many base units one contract is.
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
    bool in_delisting{false};
};

// 899 live on 2026-08 and the count only grows; past this a response is a
// venue incident or an attempt to make us allocate.
inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

// The body is a top-level ARRAY, not an object with a data member.
[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// From GET /spot/currency_pairs. Precisions are DIGIT COUNTS, not steps:
// `precision` is price decimals and `amount_precision` is quantity decimals,
// so the steps are 10^-n and a pair with amount_precision 0 trades in whole
// units.
struct WireCurrencyPair {
    std::string_view id;
    std::string_view trade_status; // tradable, untradable, buyable, sellable
    std::string_view min_base_amount;
    std::string_view min_quote_amount;
    std::uint32_t precision{0};
    std::uint32_t amount_precision{0};
};

// 2230 pairs live on 2026-08; the count only grows.
inline constexpr std::size_t k_max_currency_pairs = 8192;

struct CurrencyPairBuffer {
    std::array<WireCurrencyPair, k_max_currency_pairs> pairs{};
};

[[nodiscard]] std::expected<std::span<const WireCurrencyPair>, WireError>
parse_currency_pairs(std::string_view body, CurrencyPairBuffer& storage);

} // namespace raikiri::venues::gate
