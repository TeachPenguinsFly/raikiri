#pragma once

// The Coinbase adapter — READ ONLY, and the only SPOT venue in this tree.
//
// Market data is complete against Coinbase Exchange (the former Coinbase Pro,
// api.exchange.coinbase.com), whose public reads need no credentials at all.
// Market data only: order entry answers ErrorClass::Terminal, never Retryable.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::coinbase {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::coinbase
