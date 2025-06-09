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
// Reads any scalar as a view over the caller's buffer: a number keeps its
// token, a string loses its quotes, JSON null reads as empty. One reader for
// both because BingX QUOTES every number on the perpetual tree and sends the
// same fields bare on spot — `q:"10.00000000"` beside `q:5` for the same
// quantity, in two pushes of the same order's life.
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_scalar(ondemand::object& obj,
                                                                    std::string_view key);

// A millisecond stamp however it was spelled. Absent reads as zero, which
// map.cpp turns into local ingest time rather than into 1970.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms_any(ondemand::object& obj,
                                                                 std::string_view key);
} // namespace raikiri::venues::bingx::detail
