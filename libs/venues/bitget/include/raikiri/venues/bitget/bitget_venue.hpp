#pragma once

// The Bitget adapter.
//
// Market data only; order entry refuses loudly rather than pretending, so an
// order can never sit open waiting for a verdict no code will produce.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::bitget {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::bitget
