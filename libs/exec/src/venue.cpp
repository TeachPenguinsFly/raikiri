#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/venue.hpp>

#include <array>

namespace raikiri::exec {

namespace {

std::array<VenueFactory, k_venue_count> g_factories{};

} // namespace

void register_venue(VenueId venue, VenueFactory factory) noexcept {
    const auto idx = static_cast<std::size_t>(venue);
    if (idx > 0 && idx < g_factories.size()) {
        g_factories[idx] = factory;
    }
}

std::expected<VenueAdapter, MakeError> make_venue(const VenueConfig& config, GatewaySink& sink, const Clock& clock) {
    const auto idx = static_cast<std::size_t>(config.venue);
    if (idx == 0 || idx >= g_factories.size()) {
        return std::unexpected{MakeError::UnknownVenue};
    }
    // caps are checked before the factory lookup so a bad config is not reported as an unknown venue
    const auto& caps = caps_of(config.venue);
    if (!caps.serves(config.market)) {
        return std::unexpected{MakeError::UnsupportedMarket};
    }
    // no credentials means market data only, so the id and passphrase rules do not apply
    const bool trading = !config.api_key_env.empty() && !config.api_secret_env.empty();
    if (trading && caps.max_client_order_id_len < k_min_client_order_id_len) {
        return std::unexpected{MakeError::ClientOrderIdTooShort};
    }
    if (caps.needs_passphrase && trading && config.passphrase_env.empty()) {
        return std::unexpected{MakeError::PassphraseRequired};
    }
    if (g_factories[idx] == nullptr) {
        return std::unexpected{MakeError::UnknownVenue};
    }
    return g_factories[idx](config, sink, clock);
}

std::string_view to_string(MakeError error) noexcept {
    switch (error) {
    case MakeError::UnknownVenue:
        return "no adapter registered for this venue";
    case MakeError::UnsupportedMarket:
        return "the adapter does not serve the requested market";
    case MakeError::ClientOrderIdTooShort:
        return "venue client order ids are too short for a ULID";
    case MakeError::PassphraseRequired:
        return "this venue signs with a passphrase as well as a key and secret";
    }
    return "unknown";
}

std::string_view to_string(SubKind kind) noexcept {
    switch (kind) {
    case SubKind::Bbo:
        return "bbo";
    case SubKind::Trades:
        return "trades";
    case SubKind::Depth:
        return "depth";
    case SubKind::Funding:
        return "funding";
    case SubKind::MarkPrice:
        return "mark_price";
    case SubKind::Kline:
        return "kline";
    }
    return "unknown";
}

VenueError not_implemented(std::string_view what) noexcept {
    return VenueError::make(ErrorClass::Terminal, 0, what);
}

} // namespace raikiri::exec
