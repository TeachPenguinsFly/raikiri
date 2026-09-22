#include <raikiri/log/log.hpp>
#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
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
#include <string_view>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::kucoin {

using internal::VenueState;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;

namespace {

// Credentials arrive as references to WHERE the secret lives; the value never
// enters the config. The passphrase is sealed last on purpose: key version 2
// signs it with the api secret, so sealing before the secret is loaded would
// produce a digest keyed by nothing.
void load_credentials(const exec::VenueConfig& config, VenueState& state) {
    // Dropped on the floor, as before: a venue factory has nowhere to report a
    // misconfigured reference to, so a typo still reads downstream as a revoked
    // key. Worth fixing, and not in the same change that moves the code.
    (void)config::Credentials{config.api_key_env, config.api_secret_env}
        .key([&](std::string_view value) { state.credentials.signer().set_api_key(value); })
        .secret(state.credentials.signer().secret())
        .resolve();
    // THE PASSPHRASE IS SEALED WITH THE SECRET, so an unresolved secret leaves
    // nothing to seal it with: kucoin's header carries the passphrase encrypted
    // under the api secret, and sealing against an empty one produces a value
    // the venue rejects rather than an absent header it tolerates.
    if (!state.credentials.signer().secret().view().empty()) {
        (void)config::Credentials{{}, {}, config.passphrase_env}
            .passphrase(
                [&](std::string_view value) { (void)state.credentials.seal_passphrase(value); })
            .resolve();
    }
}

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    // THE FEED IS A DEPLOYMENT'S CHOICE, not this library's. "uta" takes the
    // unified account's `obu` channel — 4.76x fewer messages than the classic
    // tree for the same 683 contracts, measured 2026-09-19 — and needs no REST
    // seed at all. Anything else keeps classic, which is what every deployment
    // ran before this existed and what one falls back to if this feed misses.
    if (const std::string_view feed = config.market_data_feed;
        feed == "uta" || feed.starts_with("uta:")) {
        state->uta = true;
        const auto depth = feed.size() > 4 ? feed.substr(4) : k_uta_book_depth;
        state->protocol.set_uta_book_depth(depth);
        RLOG_INFO("kucoin: market data on the unified feed ({}, obu depth {})",
                  k_uta_futures_host,
                  depth);
    } else if (!config.market_data_feed.empty()) {
        RLOG_WARN("kucoin: unknown feed '{}' — staying on the classic tree",
                  config.market_data_feed);
    }
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
