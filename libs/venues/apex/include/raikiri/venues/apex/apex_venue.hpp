#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::apex {

// Explicit, from gatewayd's main: the linker drops unreferenced static
// registration objects from a static library without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::apex
