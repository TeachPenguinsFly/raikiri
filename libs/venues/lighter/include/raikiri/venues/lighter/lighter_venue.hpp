#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::lighter {

// Called from gatewayd's main: the linker drops unreferenced static registrars.
void register_adapter();

} // namespace raikiri::venues::lighter
