#pragma once

// Wire helpers specific to bingx; the shared readers live in raikiri::wire.

#include <raikiri/venues/bingx/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bingx::detail {
using namespace raikiri::wire;

// For the numbers where zero is a meaningful value rather than an absence: a
// missing `code` must not read as success, and a missing pricePrecision must
// not read as a tick size of 1.
[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj,
                                                             std::string_view key);

// "BTC-USDT@depth100@100ms" -> "BTC-USDT". The channel suffix may itself contain
// '@', so the split is on the FIRST one. Empty when there is no '@' at all.
// Not noexcept only because substr is not; every caller is inside guarded().
[[nodiscard]] std::string_view symbol_of(std::string_view data_type);
} // namespace raikiri::venues::bingx::detail
