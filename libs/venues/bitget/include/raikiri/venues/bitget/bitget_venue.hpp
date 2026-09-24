#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::bitget {

// call from gatewayd's main; the linker drops unreferenced static registration objects
void register_adapter();

} // namespace raikiri::venues::bitget
