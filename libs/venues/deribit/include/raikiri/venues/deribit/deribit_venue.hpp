#pragma once

// The Deribit adapter: market data and live order entry.
//
// Order entry covers every market this venue models — the inverse perpetual,
// the inverse dated future, and the option series that share `private/buy`
// with them. Spot is the one refusal, and it is named rather than implied.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::deribit {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::deribit
