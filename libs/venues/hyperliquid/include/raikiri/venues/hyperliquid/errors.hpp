#pragma once

// unrecognised failures default to terminal; a 200 with a `null` body is a refusal

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <string_view>

namespace raikiri::venues::hyperliquid {

using WireError = wire::Error;

[[nodiscard]] bool is_null_body(std::string_view body) noexcept;

// a 2xx with a `null` body is a refusal; a 2xx that will not parse is worth retrying
[[nodiscard]] exec::ErrorClass classify(int status, std::string_view body) noexcept;

// matched by prefix since the venue appends context; unknown reasons are terminal
[[nodiscard]] exec::ErrorClass classify_reason(std::string_view reason) noexcept;

// the order was already gone, so the cancel only arrived late
[[nodiscard]] bool is_benign_cancel_failure(std::string_view reason) noexcept;

} // namespace raikiri::venues::hyperliquid
