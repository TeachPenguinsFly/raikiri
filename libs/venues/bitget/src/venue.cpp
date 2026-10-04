#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitget {

using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

std::vector<config::CredentialError> load_credentials(const exec::VenueConfig& config, VenueState& state) {
    return config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
        .key([&](std::string_view value) { state.signer.set_api_key(value); })
        .secret(state.signer.secret())
        .passphrase([&](std::string_view value) { (void)state.passphrase.assign(value); })
        .resolve();
}

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    const auto market = config.market;
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, market);
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    const auto credential_errors = load_credentials(config, *state);
    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Bitget);
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = "api.bitget.com",
                              .target = "/api/v2/public/time",
                              .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
        std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitget, &make);
}

} // namespace raikiri::venues::bitget
