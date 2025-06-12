#pragma once

// A venue adapter is one bundle: capabilities + the three client objects,
// built by a registered factory. Explicit constructor wiring — the Go
// predecessor's 2,500-line DI factory file is exactly the ceremony this
// registry replaces. Adding a venue: implement the three interfaces, add a
// VenueCaps row, register the factory. Nothing else changes anywhere.

#include <raikiri/common/time.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_source.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace raikiri::exec {

// Runtime configuration for one gateway process. Credentials arrive as
// environment-variable NAMES (resolved by the adapter at start()), never as
// values — config files and journals must stay secret-free.
struct VenueConfig {
    VenueId venue{VenueId::Invalid};
    bool enable_spot{false};
    bool enable_perp{true};
    bool testnet{false};
    std::string api_key_env;    // e.g. "BYBIT_API_KEY"
    std::string api_secret_env; // e.g. "BYBIT_API_SECRET"
    std::string passphrase_env; // venues that need it (okx); empty otherwise
    std::string url_override;   // testing/proxying; empty = venue default
};

struct VenueAdapter {
    VenueCaps caps{};
    std::unique_ptr<DataClient> data;
    std::unique_ptr<ExecutionClient> execution;
    std::unique_ptr<InstrumentSource> instruments;
};

enum class MakeError : std::uint8_t {
    UnknownVenue,
    UnsupportedMarket,        // config enables a market the caps row denies
    ClientOrderIdTooShort,    // caps row cannot carry a 26-char ULID
    TwoMarketsAtOnce,         // one adapter, one market — see the comment below
    NoMarketSelected,         // neither flag set: nothing to load or subscribe
};

[[nodiscard]] std::string_view to_string(MakeError error) noexcept;

using VenueFactory = VenueAdapter (*)(const VenueConfig&, GatewaySink&, const Clock&);

// Registration is EXPLICIT: gatewayd's main() calls each shipped venue's
// exported register function (e.g. venues::bybit::register_adapter()).
// Deliberately no static-initialization self-registration — with static
// libraries the linker silently drops unreferenced registration objects,
// and a venue that vanishes at link time without a diagnostic is exactly
// the class of magic this codebase refuses. Single-threaded at startup.
void register_venue(VenueId venue, VenueFactory factory) noexcept;

// Builds the adapter after validating config against the caps row —
// including the ULID length gate, enforced here so a misdeclared venue
// fails at startup, loudly, not mid-trade.
[[nodiscard]] std::expected<VenueAdapter, MakeError>
make_venue(const VenueConfig& config, GatewaySink& sink, const Clock& clock);

} // namespace raikiri::exec
