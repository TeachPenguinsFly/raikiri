#include <raikiri/strategies/register.hpp>

#include <raikiri/strategies/demo_maker.hpp>
#include <raikiri/strategy/factory.hpp>

namespace raikiri::strategies {

std::size_t register_all(strategy::Factory& types) {
    // cli and api name; keep it stable when the class is renamed
    return strategy::register_type<DemoMaker>(types, "demo-maker") ? 1U : 0U;
}

std::size_t register_all() {
    return register_all(strategy::factory());
}

} // namespace raikiri::strategies
