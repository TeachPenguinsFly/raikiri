#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/paradex/endpoints.hpp>
#include <raikiri/venues/paradex/paradex_venue.hpp>

#include <memory>
#include <string>

#include "clients.hpp"

namespace raikiri::venues::paradex {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, config.budget);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Paradex);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{ep::rest_host(config.testnet)},
        .target = std::string{ep::k_time},
        .parse = [](std::string_view body) { return exec::json_number(body, "server_time", 1'000'000); }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Paradex, &make);
}

} // namespace raikiri::venues::paradex
