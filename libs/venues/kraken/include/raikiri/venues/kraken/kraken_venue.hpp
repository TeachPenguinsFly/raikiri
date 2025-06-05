#pragma once

// The Kraken adapter — Kraken FUTURES only.
//
// Market data and order entry for the perpetuals, PF_ (linear) and PI_
// (inverse). Kraken SPOT is a different product on a different host with a
// different signature, and every entry point here refuses it BY NAME rather
// than letting an id for an unloaded market read as an unknown instrument.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::kraken {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::kraken
