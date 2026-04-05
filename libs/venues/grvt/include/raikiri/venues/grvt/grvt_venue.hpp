#pragma once

// The GRVT adapter: perpetuals market data and funding, and nothing else.
//
// GRVT lists no spot market — 194 of 194 rows are perpetuals — and its order
// path signs with an EIP-712 wallet signature, so the execution client refuses
// every call rather than pretend.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::grvt {

// Explicit registration from gatewayd's main; static-init self-registration
// is banned here because the linker drops unreferenced registration objects
// from static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::grvt
