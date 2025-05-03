#pragma once

// Wire helpers specific to MEXC; the shared readers live in raikiri::wire.

#include <raikiri/venues/mexc/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::mexc::detail {
using namespace raikiri::wire;

// Reads any scalar as a view over the caller's buffer: a number keeps its
// token, a string loses its quotes, JSON null reads as empty. One reader for
// both because MEXC mixes the two encodings for the same kind of value —
// push.deal's `t` is a number and its `cts` the same instant quoted.
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_scalar(ondemand::object& obj,
                                                                    std::string_view key);

// Signed because the scale and state fields are absent-or-negative sentinels
// in the DTOs, and because the depth version chain is compared as a number.
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(ondemand::object& obj, std::string_view key, std::int64_t fallback);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
                                                             std::string_view key);

} // namespace raikiri::venues::mexc::detail
