#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/aster/aster_venue.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/rest.hpp>
#include <raikiri/venues/aster/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::aster {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, config.market);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       // the user's wallet address rides api_key_env; it is public
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       .secret([&](std::string_view value) { (void)state->wallet.load(value); })
                                       .resolve();
    state->builder.set_wallet(state->wallet, config.testnet);

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Aster);
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = std::string{rest_host(config.testnet, config.market)},
                              .target = std::string{state->builder.server_time().target},
                              .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
        std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Aster, &make);
}

} // namespace raikiri::venues::aster
