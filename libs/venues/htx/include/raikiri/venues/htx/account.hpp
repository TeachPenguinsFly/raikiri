#pragma once

// The account itself, as opposed to what it holds: the identity the spot tree
// refuses to trade without, and the swap wallet's own ledger.

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

// The `id` of the first working account of type "spot" from
// GET /v1/account/accounts. Every spot order carries it, and there is no
// default: an order without account-id is refused as an invalid parameter.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_account_id(std::string_view body);

// One row of /linear-swap-api/v3/swap_financial_record.
//
// `type` says what the row IS, as an integer: 30 is funding income and 31
// funding expenditure. The rest of the table is fees, transfers and realised
// PnL, and the table is OPEN — the live list has already dropped two codes the
// 2024 one carried and added two it did not — so an unrecognised code is
// ignored rather than treated as anything.
//
// `amount` is a JSON NUMBER with HTX's full float padding
// (`-0.024464850000000000` in the venue's own example) and it IS ALREADY
// SIGNED: negative was debited. The sign is example-derived rather than stated
// in prose, but the 30/31 split states the same direction a second time, and
// the mapper checks the two against each other rather than picking one.
//
// `id` is documented as unique per CONTRACT, not globally; `query_id` is the
// account-wide cursor and therefore the field that tells two rows apart.
struct WireFinancialRecord {
    std::string_view contract_code; // "BTC-USDT" for a swap
    std::string_view asset;         // "USDT"
    std::string_view amount;        // the raw token: a typed read loses digits
    std::uint64_t query_id{0};
    std::uint64_t ts_ms{0};
    std::int64_t type{0};
};

// The v3 body is one page cursored by `query_id`; HTX documents no size
// parameter for it, so this is sized for a generous page rather than a stated
// cap.
inline constexpr std::size_t k_max_financial_records = 512;

struct FinancialRecordBuffer {
    std::array<WireFinancialRecord, k_max_financial_records> records{};
};

// `data` is a FLAT array here. The v1 shape wrapped it in
// `data.financial_record` beside page counters, and v3 dropped both.
[[nodiscard]] std::expected<std::span<const WireFinancialRecord>, WireError>
parse_financial_records(std::string_view body, FinancialRecordBuffer& storage);

// The two `type` codes this adapter books.
inline constexpr std::int64_t k_type_funding_income = 30;
inline constexpr std::int64_t k_type_funding_expenditure = 31;

} // namespace raikiri::venues::htx
