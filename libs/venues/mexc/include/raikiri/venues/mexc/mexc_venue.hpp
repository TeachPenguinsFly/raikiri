#pragma once

// spot is write-only: spot open orders and fills refuse, so spot reconciliation is blind

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::mexc {

// registered explicitly; the linker drops unreferenced static registration objects
void register_adapter();

} // namespace raikiri::venues::mexc
