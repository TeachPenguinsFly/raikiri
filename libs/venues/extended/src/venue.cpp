#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/extended/extended_venue.hpp>

#include <memory>

#include "clients.hpp"

namespace raikiri::venues::extended {

using namespace detail;

namespace {

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, config.budget);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Extended);
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Extended, &make);
}

} // namespace raikiri::venues::extended
