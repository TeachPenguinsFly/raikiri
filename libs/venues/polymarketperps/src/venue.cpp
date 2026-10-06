#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/polymarketperps/endpoints.hpp>
#include <raikiri/venues/polymarketperps/polymarketperps_venue.hpp>

#include <memory>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::polymarketperps {

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<internal::VenueState>(sink, clock, config.budget);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::PolymarketPerps);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{ep::k_rest_host}, .target = std::string{ep::k_time}, .parse = [](std::string_view body) {
            return exec::json_number(body, "time", 1'000'000);
        }});
    adapter.data = internal::make_data_client(state);
    adapter.execution = internal::make_execution_client(state);
    adapter.instruments = internal::make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::PolymarketPerps, &make);
}

} // namespace raikiri::venues::polymarketperps
