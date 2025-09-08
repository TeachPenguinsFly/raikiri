#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/book.hpp>
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
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::kucoin {

using namespace detail;

// A definition of something DECLARED in `detail` must name it: `using
// namespace detail` only affects lookup, so an unqualified definition here
// quietly declares a second function in this namespace and the real one goes
// undefined at link.

namespace {

// Credentials arrive as references to WHERE the secret lives; the value never
// enters the config. The passphrase is sealed last on purpose: key version 2
// signs it with the api secret, so sealing before the secret is loaded would
// produce a digest keyed by nothing.
void load_credentials(const exec::VenueConfig& config, VenueState& state) {
    config::DefaultSecretProvider provider;
    const auto resolve = [&provider](std::string_view spec, config::Secret& into) {
        if (spec.empty()) {
            return false;
        }
        const auto ref = config::SecretRef::parse(spec);
        return ref.has_value() && provider.resolve(*ref, into).has_value();
    };

    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state.credentials.signer().set_api_key(key.view());
            }
        }
    }
    if (!resolve(config.api_secret_env, state.credentials.signer().secret())) {
        return;
    }
    config::Secret passphrase;
    if (resolve(config.passphrase_env, passphrase)) {
        (void)state.credentials.seal_passphrase(passphrase.view());
    }
}

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    load_credentials(config, *state);

    state->market = config.market;

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Kucoin);
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
