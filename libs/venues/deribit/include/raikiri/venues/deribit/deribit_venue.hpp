#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::deribit {

// registered from gatewayd's main: the linker drops unreferenced static-init registrars
void register_adapter();

} // namespace raikiri::venues::deribit
