#pragma once

// a venue adapter: caps plus the three clients, built by a registered factory

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
#include <vector>

namespace raikiri::exec {

// credentials arrive as environment variable names, resolved at start()
struct VenueConfig {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Perp};
    bool testnet{false};
    std::string api_key_env{};    // e.g. "BYBIT_API_KEY"
    std::string api_secret_env{}; // e.g. "BYBIT_API_SECRET"
    std::string passphrase_env{}; // venues that need it (okx); empty otherwise
    std::string url_override{};   // testing/proxying; empty = venue default

    // the venue's budget, shared by every adapter on this venue; null is unlimited
    RateBudget* budget{nullptr};

    // how long an order may go unanswered before it is reported; zero disables
    std::uint64_t order_answer_timeout_ns{0};

    // book tier to subscribe, zero for the adapter default; unsupported tiers are refused
    std::uint32_t book_depth{0};

    // which feed to take where the venue publishes several; empty is the adapter default
    std::string market_data_feed{};
};

struct VenueAdapter {
    VenueCaps caps{};
    std::unique_ptr<DataClient> data;
    std::unique_ptr<ExecutionClient> execution;
    std::unique_ptr<InstrumentSource> instruments;
    // references that were given but did not resolve; gatewayd refuses to start on any
    std::vector<std::string> credential_errors;
};

enum class MakeError : std::uint8_t {
    UnknownVenue,
    UnsupportedMarket,     // the caps row does not serve the configured market
    ClientOrderIdTooShort, // caps row cannot carry a 26-char ULID
    PassphraseRequired,    // key and secret given, but the venue signs with three
};

[[nodiscard]] std::string_view to_string(MakeError error) noexcept;

using VenueFactory = VenueAdapter (*)(const VenueConfig&, GatewaySink&, const Clock&);

// explicit registration from gatewayd's main(); the linker drops static self-registration
void register_venue(VenueId venue, VenueFactory factory) noexcept;

// validates config against the caps row, including the ulid length
[[nodiscard]] std::expected<VenueAdapter, MakeError>
make_venue(const VenueConfig& config, GatewaySink& sink, const Clock& clock);

} // namespace raikiri::exec
