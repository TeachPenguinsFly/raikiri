#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/weex_venue.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::weex {

using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->budget = config.budget;
    state->market = config.market;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       .secret(state->signer.secret())
                                       // the passphrase travels in a header, so it may be copied out
                                       .passphrase([&](std::string_view value) { state->passphrase.assign(value); })
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Weex);
    // both hosts answer {"serverTime":<ms>}
    const bool spot = config.market == MarketType::Spot;
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = std::string{spot ? ep::k_spot_rest_host : ep::k_rest_host},
                              .target = std::string{spot ? ep::k_spot_server_time : ep::k_server_time},
                              .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
        std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Weex, &make);
}

} // namespace raikiri::venues::weex
