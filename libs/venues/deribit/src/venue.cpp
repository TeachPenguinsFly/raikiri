#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::deribit {

using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    // client_id is the api key and client_secret the secret
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       .secret(state->signer.secret())
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Deribit);
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = config.testnet ? "test.deribit.com" : "www.deribit.com",
                              .target = "/api/v2/public/get_time",
                              .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
        std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Deribit, &make);
}

} // namespace raikiri::venues::deribit
