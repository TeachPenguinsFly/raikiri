#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::bybit {

// explicit registration: static libraries drop unreferenced self-registration objects
void register_adapter();

} // namespace raikiri::venues::bybit
