#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::edgex {

// Called from gatewayd's main: a static-init registration object in a static
// library is dropped by the linker without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::edgex
