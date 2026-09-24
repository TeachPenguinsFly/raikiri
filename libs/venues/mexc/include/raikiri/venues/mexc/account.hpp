#pragma once

#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// mexc does not document the sign of `funding`, so direction comes from position_type and rate
struct WireFundingRecord {
    std::string_view symbol; // "BTC_USDT", as the catalogue spells it
    std::string_view funding;
    std::string_view rate; // settlement rate, signed
    std::uint64_t id{0};
    std::uint64_t settle_ms{0};
    std::int64_t position_type{0}; // 1 long, 2 short
};

// page_size caps at 100; one poll takes one page
inline constexpr std::size_t k_max_funding_records = 128;

struct FundingRecordBuffer {
    std::array<WireFundingRecord, k_max_funding_records> records{};
};

// rows are under `data.resultList`, one level deeper than other futures bodies
[[nodiscard]] std::expected<std::span<const WireFundingRecord>, WireError>
parse_funding_records(std::string_view body, FundingRecordBuffer& storage);

} // namespace raikiri::venues::mexc
