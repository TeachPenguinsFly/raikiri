#pragma once

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::xt {

// registered from gatewayd's main; the linker drops static-init registration from static libs
void register_adapter();

} // namespace raikiri::venues::xt
