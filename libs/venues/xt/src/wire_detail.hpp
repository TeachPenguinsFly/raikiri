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

// Reads any scalar as a view over the caller's buffer: a number keeps its
// token, a string loses its quotes, JSON null reads as empty. The private
// tree needs both encodings for the SAME field — the socket quotes `price`
// while /order/list ships it as a bare number — so one reader covers them.
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(ondemand::object& obj,
                                                                    std::string_view key);

// Signed, with a caller-chosen absent value: the private DTOs use negative
// sentinels where a field may legitimately be missing.
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(ondemand::object& obj, std::string_view key, std::int64_t fallback);

[[nodiscard]] std::expected<bool, WireError>
opt_bool_or(ondemand::object& obj, std::string_view key, bool fallback);
} // namespace raikiri::venues::xt::detail
