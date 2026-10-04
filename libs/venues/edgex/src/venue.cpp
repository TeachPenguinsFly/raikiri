#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>
#include <raikiri/venues/edgex/endpoints.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::edgex {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

// no testnet host: the venue publishes no testnet rest host for its catalogue
exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.market);
    state->budget = config.budget;

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Edgex);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{ep::k_perp_rest},
        .target = "/api/v2/public/meta/getServerTime",
        .parse = [](std::string_view body) { return exec::json_number(body, "timeMillis", 1'000'000); }});
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
