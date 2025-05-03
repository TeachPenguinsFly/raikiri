#pragma once

// Wire helpers specific to HTX; the shared readers live in raikiri::wire.
//
// What differs is below — HTX puts prices, sizes and most timestamps in JSON
// numbers, so the raw-token reader is the workhorse here rather than okx's
// string readers.

#include <raikiri/venues/htx/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx::detail {
using namespace raikiri::wire;

// The raw JSON text of a value, whatever its type, quotes included. Every
// price and size goes through this: HTX writes them as numbers, and
// "1.00000000E-10" is a spelling only the token preserves.
[[nodiscard]] std::expected<std::string_view, WireError> raw_of(ondemand::value value);

// A raw token minus its quotes, if it had any: one reader for `err-code`
// whether HTX wrote "bad-request" or 2011.
[[nodiscard]] std::string_view unquote(std::string_view raw) noexcept;

// Epoch milliseconds as a digit STRING — `funding_time`, `settlement_time` and
// `settlement_date` all take that form. Never parse_decimal_e9: it scales by
// 1e9 and an epoch-ms value overflows int64 on the way.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms_str(ondemand::object& obj,
                                                                 std::string_view key);

// "market.BTC-USDT.bbo" and "public.BTC-USDT.funding_rate" both -> "BTC-USDT".
// A contract code carries a hyphen and never a dot, so it is what sits between
// the first and second separator whatever follows ("trade.detail" has a dot of
// its own). Not noexcept only because substr is not; callers are in guarded().
[[nodiscard]] std::string_view code_of(std::string_view topic);
} // namespace raikiri::venues::htx::detail
