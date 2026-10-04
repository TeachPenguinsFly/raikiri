#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::mexc {

using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       .secret(state->signer.secret())
                                       .resolve();

    state->market = config.market;

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Mexc);
    if (config.market == MarketType::Spot) {
        adapter.clock.emplace(
            exec::ServerTimeQuery{
                .host = "api.mexc.com",
                .target = "/api/v3/time",
                .parse = [](std::string_view body) { return exec::ms_to_ns(parse_spot_server_time(body)); }},
            std::shared_ptr<std::int64_t>{state, &state->spot_clock_offset_ns});
    } else {
        adapter.clock.emplace(
            exec::ServerTimeQuery{
                .host = "contract.mexc.com",
                .target = "/api/v1/contract/ping",
                .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
            std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    }
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state, config.market);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Mexc, &make);
}

} // namespace raikiri::venues::mexc
