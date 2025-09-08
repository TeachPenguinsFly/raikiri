#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
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
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    // The address rides `api_key_env` for want of a better field, and resolves
    // through the secret provider for its schemes rather than for secrecy: an
    // Ethereum address is public by construction.
    if (!config.api_key_env.empty()) {
        config::DefaultSecretProvider provider;
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret address;
            if (provider.resolve(*ref, address).has_value()) {
                state->address = std::string{address.view()};
            }
        }
    }
    // Not an HMAC secret: a secp256k1 private key, and it IS the account. A
    // gateway without it reads the account fine and can place nothing.
    if (!config.api_secret_env.empty()) {
        config::DefaultSecretProvider provider;
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                (void)state->wallet.load(key.view());
            }
        }
    }
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
