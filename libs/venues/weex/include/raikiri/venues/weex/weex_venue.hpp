#pragma once

// The WEEX adapter.
//
// Public market data — catalogue, top of book, ladder, prints and funding —
// plus a signed REST half: order entry, and the pulls a reconciliation asks.
//
// THE CAPS ROW CLAIMS NONE OF THE SIGNED HALF, and that is deliberate rather
// than an oversight. There are no WEEX credentials on this machine, so no
// signature written here has ever been accepted by the venue and no answer
// shape has ever been seen; a bit set true on an unexercised path makes a
// reconciler trust an answer nobody has looked at. libs/exec/src/caps.cpp says
// bit by bit what evidence would justify flipping each one.
//
// There is no private socket here either. WEEX publishes one and this adapter
// does not open it, so a fill reaches this process through the reconciliation
// pull and nowhere else.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::weex {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::weex
