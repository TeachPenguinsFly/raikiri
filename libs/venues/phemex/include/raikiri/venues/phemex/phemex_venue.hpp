#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::phemex {

// called from gatewayd's main; the linker drops unreferenced static registrars
void register_adapter();

} // namespace raikiri::venues::phemex
