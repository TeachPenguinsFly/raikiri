#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::hyperliquid {

using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->pulls_only = config.pulls_only;
    state->market = config.market;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    if (config.market == MarketType::Perp) {
        std::string_view feed{config.market_data_feed};
        while (!feed.empty()) {
            const auto cut = feed.find(',');
            const auto dex = feed.substr(0, cut);
            if (!dex.empty()) {
                state->dexes.emplace_back(dex);
            }
            feed = cut == std::string_view::npos ? std::string_view{} : feed.substr(cut + 1);
        }
    }
    const auto credential_errors = config::Credentials{config.api_key_env, config.api_secret_env}
                                       // the account address rides api_key_env; it is public
                                       .key([&](std::string_view value) { state->address = std::string{value}; })
                                       // secp256k1 private key; without it the gateway reads but cannot trade
                                       .secret([&](std::string_view value) { (void)state->wallet.load(value); })
                                       .resolve();
    exec::VenueAdapter adapter{};
    adapter.credential_errors = config::describe(credential_errors);
    adapter.caps = exec::caps_of(VenueId::Hyperliquid);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = "api.hyperliquid.xyz",
        .target = "/info",
        .body = R"({"type":"exchangeStatus"})",
        .parse = [](std::string_view body) { return exec::json_number(body, "time", 1'000'000); }});
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Hyperliquid, &make);
}

} // namespace raikiri::venues::hyperliquid
