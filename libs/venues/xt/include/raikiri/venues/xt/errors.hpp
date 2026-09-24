#pragma once

// unknown codes default to Terminal; futures (error.code) and spot (mc) have disjoint vocabularies;
// futures 400 is not a credential error since xt also answers it to blacklisted url keywords

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::xt {

using WireError = wire::Error;

// unknown, empty or absent symbol
inline constexpr std::string_view k_invalid_symbol = "invalid_symbol";
// unparseable parameter, e.g. level=abc
inline constexpr std::string_view k_invalid_param = "E000";
// every private spot endpoint without credentials, at http 200
inline constexpr std::string_view k_spot_missing_key = "AUTH_001";
inline constexpr std::string_view k_spot_success = "SUCCESS";

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

[[nodiscard]] exec::ErrorClass classify_spot(std::string_view code) noexcept;

// only codes naming a missing order; futures invalid_params also covers real parameter bugs
[[nodiscard]] bool is_benign_cancel_failure(std::string_view code) noexcept;

// named codes report 0; the text stays in the message
[[nodiscard]] std::int64_t numeric_code(std::string_view code) noexcept;

} // namespace raikiri::venues::xt
