#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/bitstamp/bitstamp_venue.hpp>
#include <raikiri/venues/bitstamp/endpoints.hpp>

#include <memory>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::bitstamp {

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<internal::VenueState>(sink, clock, config.market);
    state->budget = config.budget;
    // nothing signs, but a reference that does not resolve still stops start-up
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([](std::string_view) {})
                                       .secret([](std::string_view) {})
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Bitstamp);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{ep::k_host},
        .target = std::string{ep::k_server_time},
        .resolution_ns = 1'000'000'000,
        .parse = [](std::string_view body) { return exec::json_number(body, "timestamp", 1'000'000'000); }});
    adapter.data = internal::make_data_client(state);
    adapter.execution = internal::make_execution_client(state);
    adapter.instruments = internal::make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitstamp, &make);
}

} // namespace raikiri::venues::bitstamp
