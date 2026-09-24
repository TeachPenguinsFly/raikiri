#pragma once

// sim venue on the in-process matching engine; runs without api keys

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::sim {

// explicit registration; see venue.hpp
void register_adapter();

} // namespace raikiri::venues::sim
