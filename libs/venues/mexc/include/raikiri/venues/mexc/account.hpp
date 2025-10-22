#pragma once

// The account's own record of what funding cost it, off
// GET /api/v1/private/position/funding_records.

#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// One settlement, as the futures ledger states it.
//
// `funding` IS THE ONE FIELD MEXC NEVER DOCUMENTS THE SIGN OF: the response
// table calls it "Fee" and says nothing more, and its own worked example is a
// LONG settled at a NEGATIVE rate — the case where a long receives — so the
// positive figure it shows discriminates nothing. `position_type` (1 long,
// 2 short) and `rate` are documented, and between them they state the
// direction outright, which is why both are carried here and why the mapper
// reads the direction off them rather than off the amount's sign.
//
// Every numeric on this endpoint is a BARE JSON NUMBER, unlike the rest of
// this adapter's private reads, so the tokens are kept raw.
struct WireFundingRecord {
    std::string_view symbol; // "BTC_USDT", as the catalogue spells it
    std::string_view funding;
    std::string_view rate; // the funding rate the settlement used, SIGNED
    std::uint64_t id{0};
    std::uint64_t settle_ms{0};
    std::int64_t position_type{0}; // 1 long, 2 short
};

// `page_size` caps at 100 on this endpoint, and one poll takes one page.
inline constexpr std::size_t k_max_funding_records = 128;

struct FundingRecordBuffer {
    std::array<WireFundingRecord, k_max_funding_records> records{};
};

// The rows sit under `data.resultList` beside the paging counters, which is
// one level deeper than every other futures body this adapter reads.
[[nodiscard]] std::expected<std::span<const WireFundingRecord>, WireError>
parse_funding_records(std::string_view body, FundingRecordBuffer& storage);

} // namespace raikiri::venues::mexc
