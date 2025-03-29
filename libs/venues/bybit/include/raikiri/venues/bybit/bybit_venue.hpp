#pragma once

// The bybit adapter: the first live venue, implementing the same three
// contracts the sim venue does.
//
// Everything venue-specific is beneath this header — wire codecs, book
// synchronisation, signing, topic naming. What crosses it is the same
// GatewaySink and ExecutionClient the gateway already speaks to, which is
// the whole point of the contracts: adding bybit changed no code above
// libs/venues.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::bybit {

// Explicit registration from gatewayd's main. Static-init self-registration
// is banned in this codebase because the linker drops unreferenced
// registration objects from static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::bybit
