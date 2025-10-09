#pragma once

// Puts every strategy this library carries into a factory.
//
// CALLED EXPLICITLY, never left to a static initializer. A static object in a
// static library whose translation unit nothing else references is dropped by
// the linker, and the failure looks exactly like a type that silently does not
// exist: `--strategy demo-maker` answered with "no strategy type named
// demo-maker", with the code sitting right there in the tree.

#include <cstddef>

namespace raikiri::strategy {
class Factory;
}

namespace raikiri::strategies {

// How many it added. A name already taken is NOT added again — see
// Factory::add — so calling this twice adds nothing rather than replacing
// anything.
std::size_t register_all(strategy::Factory& types);

// The same, into the process factory.
std::size_t register_all();

} // namespace raikiri::strategies
