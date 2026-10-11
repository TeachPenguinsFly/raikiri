#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>
#include <raikiri/venues/edgex/endpoints.hpp>

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "clients.hpp"

namespace raikiri::venues::edgex {

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

// five values ride three references: key "<accountId>:<apiKey>", secret "<apiSecret> <tradingKey>",
// the trading key being the sdk signer's secp256k1 key from the venue's api management page
[[nodiscard]] bool load_signing(RequestBuilder& signing,
                                std::string_view key,
                                std::string_view passphrase,
                                std::string_view secret) noexcept {
    const auto cut = key.find(':');
    std::uint64_t account = 0;
    if (cut == std::string_view::npos ||
        std::from_chars(key.data(), key.data() + cut, account).ptr != key.data() + cut) {
        return false;
    }
    const auto api_secret = word(secret);
    const auto trading_key = word(secret);
    return signing.load(account, key.substr(cut + 1), passphrase, api_secret, trading_key);
}

// no testnet host: the venue's testnet sits behind its staff login
exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.market);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    std::string key;
    std::string passphrase;
    bool refused = false;
    const auto credential_errors =
        config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
            .key([&](std::string_view value) { key = std::string{value}; })
            .passphrase([&](std::string_view value) { passphrase = std::string{value}; })
            .secret([&](std::string_view value) { refused = !load_signing(state->signing, key, passphrase, value); })
            .resolve();

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    if (refused) {
        adapter.credential_errors.emplace_back(
            "edgex: key must be \"<accountId>:<apiKey>\" and secret \"<apiSecret> <tradingKey>\"");
    }
    adapter.caps = exec::caps_of(VenueId::Edgex);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{ep::k_perp_rest},
        .target = "/api/v2/public/meta/getServerTime",
        .parse = [](std::string_view body) { return exec::json_number(body, "timeMillis", 1'000'000); }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Edgex, &make);
}

} // namespace raikiri::venues::edgex
