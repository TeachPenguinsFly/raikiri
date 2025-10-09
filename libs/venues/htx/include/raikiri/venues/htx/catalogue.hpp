#pragma once

// What the venue lists, on both trees: swap contracts from api.hbdm.com and
// spot pairs from api.huobi.pro. The two state their trading limits in
// different units, which is why they are separate rows rather than one.

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

// contract_size is what one contract is worth in base units, and the only
// quantity unit HTX publishes: an order is an integer count of contracts, so
// there is no separate step. settlement_period is the funding interval in
// HOURS as a digit string ("8", "4" and "1" are all live), which okx has to
// infer from consecutive funding frames.
struct WireContract {
    std::string_view contract_code;
    std::string_view symbol;
    std::string_view contract_size;
    std::string_view price_tick;
    std::string_view business_type; // "swap" on every row this endpoint serves
    std::string_view contract_type;
    std::string_view delivery_time; // empty on a swap, an epoch on a delisting
    std::int64_t contract_status{0};
    std::int64_t settlement_period_h{0};
    std::uint64_t settlement_ms{0};
};

inline constexpr std::size_t k_max_instruments = 1024; // 276 rows live today

struct ContractBuffer {
    std::array<WireContract, k_max_instruments> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// From GET /v1/common/symbols on api.huobi.pro. Precisions are DIGIT COUNTS,
// not steps: `price-precision` is price decimals and `amount-precision` is
// quantity decimals, so the steps are 10^-n. `api-trading` is a separate gate
// from `state` — a pair can be online for the web UI and closed to keys.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view state;       // online, offline, suspend, pre-online, transfer-board
    std::string_view api_trading; // "enabled" / "disabled"
    std::string_view min_order_value;
    std::uint32_t price_precision{0};
    std::uint32_t amount_precision{0};
};

// 2159 pairs live on 2026-08 and the count only grows.
inline constexpr std::size_t k_max_spot_symbols = 8192;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::htx
