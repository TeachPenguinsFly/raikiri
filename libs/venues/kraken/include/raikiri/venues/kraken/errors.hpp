#pragma once

// errors are string names with a ": field" suffix; http 200 can still carry an error

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <string_view>

namespace raikiri::venues::kraken {

using WireError = wire::Error;

// matches up to the ": field" suffix; an empty name is terminal
[[nodiscard]] exec::ErrorClass classify(std::string_view error) noexcept;

// a cancel for an order already finished; reconciliation settles it
[[nodiscard]] bool is_benign_cancel_failure(std::string_view error) noexcept;

// the envelope says success for refused orders too; sendStatus.status is the verdict
[[nodiscard]] bool is_send_accepted(std::string_view status) noexcept;

// "filled" and "notFound" mean the order is already gone
[[nodiscard]] bool is_cancel_accepted(std::string_view status) noexcept;

[[nodiscard]] bool is_edit_accepted(std::string_view status) noexcept;

} // namespace raikiri::venues::kraken
