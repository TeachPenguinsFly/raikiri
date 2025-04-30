#pragma once

// The Coinbase adapter — READ ONLY, and the only SPOT venue in this tree.
//
// Market data is complete against Coinbase Exchange (the former Coinbase Pro,
// api.exchange.coinbase.com), whose public reads need no credentials at all.
// This venue is consumed for reference prices and basis; the framework does
// not trade it. Order entry therefore refuses permanently rather than
// temporarily, and says so, so nothing upstream waits for a verdict that will
// never come.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::coinbase {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::coinbase
