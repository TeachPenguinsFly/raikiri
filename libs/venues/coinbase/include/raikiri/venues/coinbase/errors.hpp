#pragma once

// unknown conditions default to terminal so an unknown refusal is not retried at full speed

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <string_view>

namespace raikiri::venues::coinbase {

using WireError = wire::Error;

// 200 and 404 seen live; the rest follow the docs
[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

// "Failed to subscribe" means a bad product id; retrying gives the same refusal
[[nodiscard]] exec::ErrorClass classify_stream(std::string_view message) noexcept;

} // namespace raikiri::venues::coinbase
