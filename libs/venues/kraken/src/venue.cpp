#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/rest.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::kraken {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

std::optional<std::uint64_t> futures_server_time(std::string_view body) {
    constexpr std::string_view key = R"("serverTime":")";
    const auto at = body.find(key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const auto from = at + key.size();
    const auto to = body.find('"', from);
    if (to == std::string_view::npos) {
        return std::nullopt;
    }
    return exec::ms_to_ns(parse_iso8601_ms(body.substr(from, to - from)));
}

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, config.market);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    // the secret stays in base64; sign.hpp decodes it per signature
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       .secret(state->signer.secret())
                                       .resolve();
    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Kraken);
    if (state->spot()) {
        adapter.clock.emplace(exec::ServerTimeQuery{
            .host = "api.kraken.com",
            .target = "/0/public/Time",
            .resolution_ns = 1'000'000'000,
            .parse = [](std::string_view body) { return exec::json_number(body, "unixtime", 1'000'000'000); }});
    } else {
        adapter.clock.emplace(exec::ServerTimeQuery{.host = "futures.kraken.com",
                                                    .target = "/derivatives/api/v3/instruments/status",
                                                    .parse = futures_server_time},
                              std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    }
    adapter.data = state->spot() ? make_spot_data_client(state) : make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Kraken, &make);
}

} // namespace raikiri::venues::kraken
