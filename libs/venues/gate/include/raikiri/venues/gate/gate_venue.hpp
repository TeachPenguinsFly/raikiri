#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::gate {

// called from gatewayd's main; the linker drops unreferenced static registrars
void register_adapter();

} // namespace raikiri::venues::gate
