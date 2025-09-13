#pragma once

// The account's own book: every row that moved money, of which funding and
// trading fees are the two this process reads.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// One row of GET /api/v5/account/bills.
//
// `type` says what the row IS — "8" funding, "2" a trade — and the two put
// their money in DIFFERENT COLUMNS: a funding row states it in `balChg` and
// leaves `fee` at zero, a trade row the reverse. Reading the wrong column
// books a zero over a real charge, which is the whole reason both are carried.
struct WireBill {
    std::string_view bill_id;
    std::string_view inst_id;
    std::string_view type;
    std::string_view bal_chg;
    std::string_view fee;
    std::string_view trade_id;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_bills = 256;

struct BillBuffer {
    std::array<WireBill, k_max_bills> bills{};
};

[[nodiscard]] std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                              BillBuffer& storage);

} // namespace raikiri::venues::okx
