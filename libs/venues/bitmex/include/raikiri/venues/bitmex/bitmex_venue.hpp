#pragma once

// The BitMEX adapter — spot and the perpetuals.
//
// Market data and order entry for both. Dated futures price correctly here but
// VenueConfig carries no switch that would load them, and options are not
// listed at all; both are refused BY NAME rather than letting an id for an
// unloaded market read as an unknown instrument.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::bitmex {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::bitmex
