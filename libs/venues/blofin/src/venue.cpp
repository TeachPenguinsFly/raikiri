#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/blofin/blofin_venue.hpp>

#include <memory>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::blofin {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    // blofin signs with a passphrase as well; without one every signed call is refused
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       .secret(state->signer.secret())
                                       .passphrase([&](std::string_view value) { state->passphrase.assign(value); })
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Blofin);
    // no time endpoint: the busiest book's stamp, which trails the server clock by milliseconds
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = "openapi.blofin.com",
                              .target = "/api/v1/market/books?instId=BTC-USDT&size=1",
                              .parse = [](std::string_view body) { return exec::json_number(body, "ts", 1'000'000); }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Blofin, &make);
}

} // namespace raikiri::venues::blofin
