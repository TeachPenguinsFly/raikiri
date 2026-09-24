#pragma once

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

// every spot order carries this id and is refused without it
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_account_id(std::string_view body);

// type 30/31 is funding income/expenditure and amount is already signed; query_id is the account-wide unique id
struct WireFinancialRecord {
    std::string_view contract_code; // "BTC-USDT" for a swap
    std::string_view asset;         // "USDT"
    std::string_view amount;        // the raw token: a typed read loses digits
    std::uint64_t query_id{0};
    std::uint64_t ts_ms{0};
    std::int64_t type{0};
};

inline constexpr std::size_t k_max_financial_records = 512;

struct FinancialRecordBuffer {
    std::array<WireFinancialRecord, k_max_financial_records> records{};
};

[[nodiscard]] std::expected<std::span<const WireFinancialRecord>, WireError>
parse_financial_records(std::string_view body, FinancialRecordBuffer& storage);

inline constexpr std::int64_t k_type_funding_income = 30;
inline constexpr std::int64_t k_type_funding_expenditure = 31;

} // namespace raikiri::venues::htx
