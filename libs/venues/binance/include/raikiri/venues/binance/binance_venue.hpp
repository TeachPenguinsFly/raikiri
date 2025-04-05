#pragma once

// The binance adapter: the second live venue.
//
// Market data is complete; order entry is not. Every ExecutionClient method
// refuses loudly rather than pretending — a gateway started against binance
// streams real prices and rejects any order with a terminal event, which is
// the only honest state for an adapter whose signing path is unwritten.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::binance {

// Explicit registration from gatewayd's main; static-init self-registration
// is banned here because the linker drops unreferenced registration objects
// from static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::binance
