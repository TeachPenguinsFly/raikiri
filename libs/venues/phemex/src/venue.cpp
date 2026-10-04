#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/phemex/phemex_venue.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::phemex {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Phemex);
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = "api.phemex.com", .target = "/public/time", .parse = [](std::string_view body) {
                                  return exec::json_number(body, "serverTime", 1'000'000);
                              }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Phemex, &make);
}

} // namespace raikiri::venues::phemex
