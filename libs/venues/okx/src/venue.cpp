#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/okx_venue.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::okx {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

std::vector<config::CredentialError> load_credentials(VenueState& state, const exec::VenueConfig& config) {
    // okx needs the passphrase; without it signed requests get 50104
    return config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
        .key([&](std::string_view value) { state.signer.set_api_key(value); })
        .secret(state.signer.secret())
        .passphrase([&](std::string_view value) { (void)state.passphrase.assign(value); })
        .resolve();
}

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->pulls_only = config.pulls_only;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    state->spot_enabled = config.market == MarketType::Spot;
    state->perp_enabled = config.market == MarketType::Perp;
    state->option_enabled = config.market == MarketType::Option;
    state->families = config.market_data_feed;
    const auto credential_errors = load_credentials(*state, config);

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Okx);
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = "www.okx.com",
                              .target = "/api/v5/public/time",
                              .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
        std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Okx, &make);
}

} // namespace raikiri::venues::okx
