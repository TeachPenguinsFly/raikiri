#pragma once

// The Coinbase adapter: market data and live spot order entry.
//
// Coinbase Exchange (the former Coinbase Pro, api.exchange.coinbase.com),
// whose public reads need no credentials at all — order entry needs three.
//
// SPOT is the whole of it, and that is the entity rather than a choice: this
// one lists no perpetual, no dated future and no option, so each of those is
// refused by name. Executions are the one thing order entry cannot observe in
// real time, and fill_reports says so instead of answering empty.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::coinbase {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::coinbase
