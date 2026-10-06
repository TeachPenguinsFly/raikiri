#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/upbit/endpoints.hpp>
#include <raikiri/venues/upbit/upbit_venue.hpp>

#include <memory>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::upbit {

exec::VenueAdapter
make_adapter(const Flavour& flavour, const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<internal::VenueState>(flavour, sink, clock);
    state->budget = config.budget;
    // nothing signs, but a reference that does not resolve still stops start-up
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([](std::string_view) {})
                                       .secret([](std::string_view) {})
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(flavour.venue);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{flavour.rest_host},
        .target = std::string{ep::k_server_time},
        .parse = [](std::string_view body) { return exec::json_number(body, "timestamp", 1'000'000); }});
    adapter.data = internal::make_data_client(state);
    adapter.execution = internal::make_execution_client(state);
    adapter.instruments = internal::make_instrument_source(state);
    return adapter;
}

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    return make_adapter(k_upbit, config, sink, clock);
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Upbit, &make);
}

} // namespace raikiri::venues::upbit
