#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/bullish/bullish_venue.hpp>
#include <raikiri/venues/bullish/endpoints.hpp>

#include <memory>
#include <string>

#include "clients.hpp"

namespace raikiri::venues::bullish {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, config.budget);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bullish);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{ep::host(config.testnet)},
        .target = std::string{ep::k_time},
        .parse = [](std::string_view body) { return exec::json_number(body, "timestamp", 1'000'000); }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bullish, &make);
}

} // namespace raikiri::venues::bullish
