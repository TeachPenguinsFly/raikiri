#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::kucoin {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

// credentials are references to where secrets live; seal the passphrase after the secret loads
std::vector<config::CredentialError> load_credentials(const exec::VenueConfig& config, VenueState& state) {
    auto errors = config::Credentials{config.api_key_env, config.api_secret_env}
                      .key([&](std::string_view value) { state.credentials.signer().set_api_key(value); })
                      .secret(state.credentials.signer().secret())
                      .resolve();
    // the passphrase is sealed with the secret, so skip sealing without one
    if (!state.credentials.signer().secret().view().empty()) {
        const auto sealed = config::Credentials{{}, {}, config.passphrase_env}
                                .passphrase(
                                    [&](std::string_view value) { (void)state.credentials.seal_passphrase(value); })
                                .resolve();
        errors.insert(errors.end(), sealed.begin(), sealed.end());
    }
    return errors;
}

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->budget = config.budget;
    state->pulls_only = config.pulls_only;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    // "uta" selects the obu feed; anything else keeps classic
    if (const std::string_view feed = config.market_data_feed; feed == "uta" || feed.starts_with("uta:")) {
        state->uta = true;
        const auto depth = feed.size() > 4 ? feed.substr(4) : k_uta_book_depth;
        state->protocol.set_uta_book_depth(depth);
        RLOG_INFO("kucoin: market data on the unified feed ({}, obu depth {})", k_uta_futures_host, depth);
    } else if (!config.market_data_feed.empty()) {
        RLOG_WARN("kucoin: unknown feed '{}' — staying on the classic tree", config.market_data_feed);
    }
    const auto credential_errors = load_credentials(config, *state);

    state->market = config.market;

    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Kucoin);
    if (config.market == MarketType::Spot) {
        adapter.clock.emplace(
            exec::ServerTimeQuery{
                .host = "api.kucoin.com",
                .target = "/api/v1/timestamp",
                .parse = [](std::string_view body) { return exec::ms_to_ns(parse_server_time(body)); }},
            std::shared_ptr<std::int64_t>{state, &state->clock_offset_ns});
    } else {
        adapter.clock.emplace(
            exec::ServerTimeQuery{
                .host = "api-futures.kucoin.com",
                .target = "/api/v1/timestamp",
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
    exec::register_venue(VenueId::Kucoin, &make);
}

} // namespace raikiri::venues::kucoin
