#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::bitfinex {

// registered explicitly from gatewayd's main; the linker drops unreferenced static registrars
void register_adapter();

} // namespace raikiri::venues::bitfinex
