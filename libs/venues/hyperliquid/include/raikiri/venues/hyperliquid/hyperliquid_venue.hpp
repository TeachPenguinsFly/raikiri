#pragma once

// The Hyperliquid adapter — READ ONLY.
//
// This venue is consumed for reference prices, basis and funding comparison;
// the framework does not trade it. Order entry therefore refuses permanently
// rather than temporarily, and says so, so nothing upstream waits for a
// verdict that will never come.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::hyperliquid {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::hyperliquid
