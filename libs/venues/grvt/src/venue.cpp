#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/grvt/grvt_venue.hpp>

#include <memory>

#include "clients.hpp"

namespace raikiri::venues::grvt {

using namespace detail;

namespace {

// No credentials are read, since nothing this adapter sends is signed, and no
// rate budget is taken: its one REST call is the catalogue, once per start.
exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Grvt);
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Grvt, &make);
}

} // namespace raikiri::venues::grvt
