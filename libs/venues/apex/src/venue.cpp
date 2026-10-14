#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/apex/apex_venue.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::apex {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

[[nodiscard]] std::string_view word(std::string_view& text) noexcept {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        text = {};
        return {};
    }
    text.remove_prefix(begin);
    const auto end = std::min(text.find_first_of(" \t\r\n"), text.size());
    const auto out = text.substr(0, end);
    text.remove_prefix(end);
    return out;
}

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    // four values ride three references: the secret is "<apiSecret> <zkSeeds>", the seeds in hex
    std::string key;
    std::string passphrase;
    bool refused = false;
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
                                       .key([&](std::string_view value) { key = std::string{value}; })
                                       .passphrase([&](std::string_view value) { passphrase = std::string{value}; })
                                       .secret([&](std::string_view value) {
                                           const auto api_secret = word(value);
                                           const auto seeds = word(value);
                                           refused = !state->signing.load(key, passphrase, api_secret, seeds);
                                       })
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    if (refused) {
        adapter.credential_errors.emplace_back("apex: secret must be \"<apiSecret> <zkSeeds>\", the seeds in hex");
    }
    adapter.caps = exec::caps_of(VenueId::Apex);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = "omni.apex.exchange", .target = "/api/v3/time", .parse = [](std::string_view body) {
            return exec::json_number(body, "time", 1'000'000);
        }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Apex, &make);
}

} // namespace raikiri::venues::apex
