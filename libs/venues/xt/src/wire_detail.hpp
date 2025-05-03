#pragma once

// Wire helpers specific to XT; the shared readers live in raikiri::wire.
//
// What differs is below — XT puts its timestamps in JSON numbers and its
// sequence ids in digit strings, so both a numeric and a textual reader are
// needed where okx needed only the textual one.

#include <raikiri/venues/xt/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::xt::detail {
using namespace raikiri::wire;

[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj,
                                                             std::string_view key);
[[nodiscard]] std::expected<bool, WireError> req_bool(ondemand::object& obj, std::string_view key);

// XT's `t` is epoch milliseconds as a JSON number. Reading it as a number
// keeps it away from parse_decimal_e9, which scales by 1e9 and overflows int64
// on an epoch-ms value.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
                                                             std::string_view key);

// Sequence ids (`u`, `pu`, `fu`, `id`) are digit STRINGS. Anything that is not
// exactly a run of digits yields 0.
[[nodiscard]] std::expected<std::uint64_t, WireError> req_digits(ondemand::object& obj,
                                                                 std::string_view key);
} // namespace raikiri::venues::xt::detail
