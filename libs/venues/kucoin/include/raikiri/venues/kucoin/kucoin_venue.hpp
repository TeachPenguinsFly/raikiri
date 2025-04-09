#pragma once

// The Kucoin adapter.
//
// Market data only for now; order entry refuses loudly rather than pretending,
// so an order can never sit open waiting for a verdict no code will produce.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::kucoin {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::kucoin
