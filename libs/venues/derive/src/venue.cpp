#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/derive/derive_venue.hpp>

#include <memory>

#include "clients.hpp"

namespace raikiri::venues::derive {

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<internal::VenueState>(sink, clock, config.market);
    state->budget = config.budget;
    state->underlyings = config.market_data_feed;
    // nothing signs yet, but a reference that does not resolve still stops start-up
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([](std::string_view) {})
                                       .secret([](std::string_view) {})
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Derive);
    adapter.data = internal::make_data_client(state);
    adapter.execution = internal::make_execution_client(state);
    adapter.instruments = internal::make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Derive, &make);
}

} // namespace raikiri::venues::derive
