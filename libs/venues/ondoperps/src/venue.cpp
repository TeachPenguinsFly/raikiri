#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/ondoperps/endpoints.hpp>
#include <raikiri/venues/ondoperps/ondoperps_venue.hpp>
#include <raikiri/venues/ondoperps/wire.hpp>

#include <memory>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::ondoperps {

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<internal::VenueState>(sink, clock, config.budget);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::OndoPerps);
    adapter.clock.emplace(exec::ServerTimeQuery{.host = std::string{ep::k_host},
                                                .target = std::string{ep::k_time},
                                                .resolution_ns = 1'000,
                                                .parse = &server_time});
    adapter.data = internal::make_data_client(state);
    adapter.execution = internal::make_execution_client(state);
    adapter.instruments = internal::make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::OndoPerps, &make);
}

} // namespace raikiri::venues::ondoperps
