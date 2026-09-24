#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::aster {

// explicit registration: static libraries drop unreferenced self-registration objects
void register_adapter();

} // namespace raikiri::venues::aster
