#pragma once

// kraken futures only; spot is refused by name

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::kraken {

// call from gatewayd's main; the linker drops unreferenced static registration objects
void register_adapter();

} // namespace raikiri::venues::kraken
