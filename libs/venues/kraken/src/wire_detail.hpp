#pragma once

// Wire helpers specific to Kraken; the shared readers live in raikiri::wire.

#include <raikiri/venues/kraken/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kraken::detail {
using namespace raikiri::wire;

// Reads a JSON NUMBER as raw token text. Kraken emits `64110.0` and
// `8.907125e-6` for the same field on one socket, so nothing here interprets
// it: map.cpp owns the parser that reads both. JSON null reads as empty.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(ondemand::object& obj, std::string_view key, bool& present);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
                                                             std::string_view key);

} // namespace raikiri::venues::kraken::detail
