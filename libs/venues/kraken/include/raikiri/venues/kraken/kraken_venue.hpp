#pragma once

// The Kraken adapter — READ ONLY.
//
// Market data only: order entry answers ErrorClass::Terminal, never Retryable.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::kraken {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::kraken
