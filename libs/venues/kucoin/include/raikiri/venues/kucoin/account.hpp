#pragma once

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// numbers carry more digits than FixedE9; accountEquity is omitted as marginBalance plus unrealisedPNL
struct WireAccountOverview {
    std::string_view currency;
    std::string_view margin_balance;    // excludes unrealised pnl
    std::string_view available_balance; // what a new order may commit
    std::string_view unrealised_pnl;
};

[[nodiscard]] std::expected<WireAccountOverview, WireError> parse_account_overview(std::string_view body);

// one settlement on one symbol; funding is signed as the account moved, positive received
struct WireFundingCharge {
    std::uint64_t id{0};
    std::string_view symbol;
    std::string_view funding;
    std::string_view settle_currency;
    std::uint64_t time_ms{0};
};

inline constexpr std::size_t k_funding_page = 100;

struct FundingChargeBuffer {
    std::array<WireFundingCharge, k_funding_page> charges{};
};

struct WireFundingPage {
    std::span<const WireFundingCharge> charges;
    bool has_more{false};
};

// GET /api/v1/funding-history: data.dataList and data.hasMore
[[nodiscard]] std::expected<WireFundingPage, WireError> parse_funding_history(std::string_view body,
                                                                              FundingChargeBuffer& storage);

} // namespace raikiri::venues::kucoin
