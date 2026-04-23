#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::dydx {

// Called from gatewayd's main: the linker drops a static library's
// self-registration objects without a word.
void register_adapter();

} // namespace raikiri::venues::dydx
