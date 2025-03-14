#pragma once

// The sim venue adapter: the 9th "exchange", implementing the exact same
// contracts as the live eight, backed by the in-process matching engine.
// This is what makes the repo runnable without API keys — and it exercises
// the ENTIRE adapter surface (order lifecycle, maker fills, reconciliation
// reports), so the contracts are proven before any live venue exists.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::sim {

// Explicit registration (called from gatewayd main — see venue.hpp for why
// static-init self-registration is banned).
void register_adapter();

} // namespace raikiri::venues::sim
