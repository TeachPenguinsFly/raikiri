#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::phemex {

// Called from gatewayd's main: the linker drops static-init registration
// objects from static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::phemex
