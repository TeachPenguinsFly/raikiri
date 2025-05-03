#pragma once

// Wire helpers specific to Deribit; the shared readers live in raikiri::wire.

#include <raikiri/venues/deribit/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::deribit::detail {
using namespace raikiri::wire;

// Reads a JSON number as its raw token, likewise over the caller's buffer, in
// whatever form Deribit chose ("0.5", "2.7e4"); decoding is parse_number_e9's
// job at the mapping edge.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(ondemand::object& obj,
                                                                 std::string_view key);

// Epoch milliseconds. These must NOT go through parse_number_e9: it scales by
// 1e9 and an epoch-ms value overflows int64 on the way. std::from_chars over
// the raw token is the whole conversion; anything else yields 0, which the
// mapper reads as "venue gave us no time" and replaces with ingest time.
[[nodiscard]] std::uint64_t token_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
                                                             std::string_view key);

} // namespace raikiri::venues::deribit::detail
