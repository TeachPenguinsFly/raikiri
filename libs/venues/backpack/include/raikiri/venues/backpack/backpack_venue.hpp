#pragma once

// The backpack adapter: spot and perpetual market data, and funding.
//
// Order entry is refused, not missing: a gateway started against backpack
// streams real prices and rejects any order with a terminal event, because the
// venue signs private requests with an ED25519 key this codebase has no signer
// for.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::backpack {

// Explicit registration from gatewayd's main; static-init self-registration
// is banned here because the linker drops unreferenced registration objects
// from static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::backpack
