#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>

#include <memory>

#include "clients.hpp"

namespace raikiri::venues::edgex {

using namespace detail;

namespace {

// No testnet host is read: the venue publishes a testnet quote socket but no
// REST host for its catalogue.
exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.market);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Edgex);
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Edgex, &make);
}

} // namespace raikiri::venues::edgex
