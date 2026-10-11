#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::htx {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       .secret(state->signer.secret())
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Htx);
    if (config.market == MarketType::Spot) {
        adapter.clock.emplace(
            exec::ServerTimeQuery{
                .host = "api-aws.huobi.pro",
                .target = "/v1/common/timestamp",
                .parse = [](std::string_view body) { return exec::json_number(body, "data", 1'000'000); }},
            std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    } else {
        adapter.clock.emplace(
            exec::ServerTimeQuery{
                .host = "api.hbdm.vn",
                .target = "/api/v1/timestamp",
                .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
            std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    }
    adapter.data = make_data_client(state, config.market);
    adapter.execution = make_execution_client(state, config.market);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Htx, &make);
}

} // namespace raikiri::venues::htx
