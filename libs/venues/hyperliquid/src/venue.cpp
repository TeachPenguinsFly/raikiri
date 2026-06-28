#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
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

using namespace detail;

namespace {

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
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
    // Dropped on the floor, as before: a venue factory has nowhere to report a
    // misconfigured reference to, so a typo still reads downstream as a revoked
    // key. Worth fixing, and not in the same change that moves the code.
    (void)config::Credentials{config.api_key_env, config.api_secret_env}
        // The address rides `api_key_env` for want of a better field, and goes
        // through the provider for its schemes rather than for secrecy: an
        // Ethereum address is public by construction.
        .key([&](std::string_view value) { state->address = std::string{value}; })
        // Not an HMAC secret: a secp256k1 private key, and it IS the account. A
        // gateway without it reads the account fine and can place nothing.
        .secret([&](std::string_view value) { (void)state->wallet.load(value); })
        .resolve();
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Hyperliquid);
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
