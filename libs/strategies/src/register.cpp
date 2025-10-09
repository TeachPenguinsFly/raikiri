#include <raikiri/strategies/register.hpp>

#include <raikiri/strategies/demo_maker.hpp>
#include <raikiri/strategy/factory.hpp>

namespace raikiri::strategies {

std::size_t register_all(strategy::Factory& types) {
    // A WIRE NAME, not the class name. It is typed on a command line and sent
    // in a request body, so it is lowercase and hyphenated and does not change
    // when the class is renamed.
    return strategy::register_type<DemoMaker>(types, "demo-maker") ? 1U : 0U;
}

std::size_t register_all() {
    return register_all(strategy::factory());
}

} // namespace raikiri::strategies
