#pragma once

// The lighter adapter: a zk-rollup perpetual DEX that also lists spot.
//
// Market data and funding are complete; order entry is not built. Every
// ExecutionClient method refuses with a terminal answer, which is the honest
// state for an adapter whose signer does not exist.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::lighter {

// Explicit registration from gatewayd's main; static-init self-registration
// is banned here because the linker drops unreferenced registration objects
// from static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::lighter
