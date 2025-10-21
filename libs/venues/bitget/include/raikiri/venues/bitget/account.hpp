#pragma once

// The account's own book: what the mix wallet was charged, one row per event.

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// One row of /api/v2/mix/account/bill.
//
// `businessType` says what the row IS, and `contract_settle_fee` is the only
// funding value the classic v2 enum has — there is no `funding_fee` here, and
// the uppercase `*_SETTLE_FEE_USER_IN`/`_OUT` pair belongs to the separate UTA
// v3 ledger.
//
// TWO MONEY COLUMNS, AND THEY ARE NOT INTERCHANGEABLE. `amount` carries the
// settlement and `fee` the trading commission, and each is zero on the other's
// rows. `fee` is deliberately not read: a commission is already booked from
// the fill that incurred it, and the copy exists to be COMPARED — keyed by the
// venue's id for the TRADE, which a bill row does not carry at all. Booking it
// would double-charge, and comparing it would key on nothing.
//
// `amount` IS ALREADY SIGNED: the venue's own worked example settles
// `-0.004992` against a balance that fell by it. Bitget states no sign rule in
// prose, so this is example-derived rather than promised, which is why the
// mapper takes the figure verbatim rather than deriving a direction it would
// then have to agree with.
struct WireBill {
    std::string_view bill_id;
    std::string_view symbol; // "BTCUSDT", as the catalogue spells it
    std::string_view coin;
    std::string_view business_type;
    std::string_view amount;
    std::uint64_t c_time_ms{0};
};

// `limit` caps at 100 on this endpoint, and one poll takes one page.
inline constexpr std::size_t k_max_bills = 128;

struct BillBuffer {
    std::array<WireBill, k_max_bills> bills{};
};

// The rows sit under `data.bills` beside the `endId` cursor, one level deeper
// than the fills and positions bodies.
[[nodiscard]] std::expected<std::span<const WireBill>, WireError>
parse_mix_bills(std::string_view body, BillBuffer& storage);

} // namespace raikiri::venues::bitget
