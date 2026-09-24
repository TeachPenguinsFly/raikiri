#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::weex {

// explicit: the linker drops unreferenced static-init registrations from static libraries
void register_adapter();

} // namespace raikiri::venues::weex
