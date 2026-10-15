#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/lighter/lighter_venue.hpp>

#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::lighter {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

// "account:key", the account index and the api key slot it signs with
bool parse_account(std::string_view value, VenueState& state) {
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const auto account = value.substr(0, colon);
    const auto key = value.substr(colon + 1);
    std::int64_t account_index = -1;
    unsigned key_index = 256;
    const auto [a_end, a_ec] = std::from_chars(account.data(), account.data() + account.size(), account_index);
    const auto [k_end, k_ec] = std::from_chars(key.data(), key.data() + key.size(), key_index);
    if (a_ec != std::errc{} || k_ec != std::errc{} || a_end != account.data() + account.size() ||
        k_end != key.data() + key.size() || account_index < 0 || key_index > 254) {
        return false;
    }
    state.account_index = account_index;
    state.api_key_index = static_cast<std::uint8_t>(key_index);
    state.account_parsed = true;
    return true;
}

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->rest_budget = config.budget;
    state->pulls_only = config.pulls_only;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    bool account_ok = true;
    bool key_ok = true;
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       .key([&](std::string_view value) { account_ok = parse_account(value, *state); })
                                       .secret([&](std::string_view value) { key_ok = state->key.load(value); })
                                       .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    if (!account_ok) {
        adapter.credential_errors.emplace_back("lighter: the key reference must give account_index:api_key_index");
    }
    if (!key_ok) {
        adapter.credential_errors.emplace_back("lighter: the secret reference must give a 40-byte hex api private key");
    }
    adapter.caps = exec::caps_of(VenueId::Lighter);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = "mainnet.zklighter.elliot.ai",
        .target = "/",
        .resolution_ns = 1'000'000'000,
        .parse = [](std::string_view body) { return exec::json_number(body, "timestamp", 1'000'000'000); }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Lighter, &make);
}

} // namespace raikiri::venues::lighter
