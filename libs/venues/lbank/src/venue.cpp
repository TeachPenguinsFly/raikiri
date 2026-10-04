#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/lbank/lbank_venue.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::lbank {

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<internal::VenueState>(sink, clock, config.market);
    state->budget = config.budget;
    bool key = false;
    bool secret = false;
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([&](std::string_view value) { key = !value.empty(); })
                                       .secret([&](std::string_view value) { secret = !value.empty(); })
                                       .resolve();
    state->has_credentials = key && secret;

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Lbank);
    if (config.market == MarketType::Spot) {
        adapter.clock.emplace(exec::ServerTimeQuery{
            .host = "api.lbank.info", .target = "/v2/timestamp.do", .parse = [](std::string_view body) {
                return exec::json_number(body, "data", 1'000'000);
            }});
    } else {
        adapter.clock.emplace(exec::ServerTimeQuery{
            .host = "lbkperp.lbank.com", .target = "/cfd/openApi/v1/pub/getTime", .parse = [](std::string_view body) {
                return exec::json_number(body, "data", 1'000'000);
            }});
    }
    adapter.data = internal::make_data_client(state);
    adapter.execution = internal::make_execution_client(state);
    adapter.instruments = internal::make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Lbank, &make);
}

} // namespace raikiri::venues::lbank
