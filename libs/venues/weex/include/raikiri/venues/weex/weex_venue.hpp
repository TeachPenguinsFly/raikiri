#pragma once

// The WEEX adapter.
//
// PUBLIC MARKET DATA ONLY: catalogue, top of book, ladder, prints and funding.
// There is no order entry and no private stream here, and the caps row declares
// both false — an adapter that claimed either would be accepted at registration
// and then fail one command at a time.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::weex {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::weex
