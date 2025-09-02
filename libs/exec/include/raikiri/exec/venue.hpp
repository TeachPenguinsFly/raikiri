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
#include <raikiri/exec/rate_budget.hpp>

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
    // ONE MARKET. It used to be two booleans, which could say "both" and
    // "neither" — neither of which an adapter can serve, since every one of
    // them collapses to a single MarketType and never looks again. make_venue
    // grew two error arms to refuse those combinations; one field cannot
    // express either, so the refusals are gone with them.
    MarketType market{MarketType::Perp};
    bool testnet{false};
    std::string api_key_env;    // e.g. "BYBIT_API_KEY"
    std::string api_secret_env; // e.g. "BYBIT_API_SECRET"
    std::string passphrase_env; // venues that need it (okx); empty otherwise
    std::string url_override;   // testing/proxying; empty = venue default

    // The venue's published budget, owned by the process and SHARED by every
    // adapter pointed at this venue — the order path and the account poller
    // both spend it, because the venue counts them together against one IP.
    // Null means unlimited, which is the honest reading of a venue nobody has
    // measured; the numbers live in config/venues.yaml, never in the binary.
    RateBudget* budget{nullptr};

    // How long an order may go unanswered before the adapter reports it as a
    // gap. Zero disables the sweep, which is the wrong setting for money.
    std::uint64_t order_answer_timeout_ns{0};
};

struct VenueAdapter {
    VenueCaps caps{};
    std::unique_ptr<DataClient> data;
    std::unique_ptr<ExecutionClient> execution;
    std::unique_ptr<InstrumentSource> instruments;
};

enum class MakeError : std::uint8_t {
    UnknownVenue,
    UnsupportedMarket,     // the caps row does not serve the configured market
    ClientOrderIdTooShort, // caps row cannot carry a 26-char ULID
    PassphraseRequired,    // key and secret given, but the venue signs with three
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
