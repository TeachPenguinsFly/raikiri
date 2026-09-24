#pragma once

// called explicitly, since the linker drops an unreferenced static initializer

#include <cstddef>

namespace raikiri::strategy {
class Factory;
}

namespace raikiri::strategies {

// returns how many it added; a taken name is skipped
std::size_t register_all(strategy::Factory& types);

// The same, into the process factory.
std::size_t register_all();

} // namespace raikiri::strategies
