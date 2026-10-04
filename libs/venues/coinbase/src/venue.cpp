#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/coinbase/envelope.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/rest.hpp>
#include <raikiri/venues/coinbase/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::coinbase {

using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
                                       .key([&](std::string_view value) { state->signer.set_api_key(value); })
                                       // decoded once: coinbase keys the hmac with the raw secret bytes
                                       .secret([&](std::string_view encoded) {
                                           std::array<char, k_max_secret_bytes> raw{};
                                           if (const auto len = decode_secret(encoded, raw); len.has_value()) {
                                               (void)state->signer.secret().assign(std::string_view{raw.data(), *len});
                                           }
                                       })
                                       .passphrase(
                                           [&](std::string_view value) { state->passphrase = std::string{value}; })
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Coinbase);
    adapter.clock.emplace(
        exec::ServerTimeQuery{.host = "api.exchange.coinbase.com",
                              .target = "/time",
                              .parse = [](std::string_view body) { return exec::json_seconds(body, "epoch"); }},
        std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Coinbase, &make);
}

} // namespace raikiri::venues::coinbase
