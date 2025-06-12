#include <raikiri/exec/venue.hpp>
#include <raikiri/exec/execution_client.hpp>

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

std::expected<VenueAdapter, MakeError>
make_venue(const VenueConfig& config, GatewaySink& sink, const Clock& clock) {
    const auto idx = static_cast<std::size_t>(config.venue);
    if (idx == 0 || idx >= g_factories.size() || g_factories[idx] == nullptr) {
        return std::unexpected{MakeError::UnknownVenue};
    }
    const auto& caps = caps_of(config.venue);
    if ((config.enable_spot && !caps.spot) || (config.enable_perp && !caps.perp)) {
        return std::unexpected{MakeError::UnsupportedMarket};
    }
    // ONE MARKET PER ADAPTER. Several adapters collapse the two flags to a
    // single MarketType and then never look again — bybit and bitget do it
    // literally, `enable_spot ? Spot : Perp` — so enabling both does not host
    // both: spot wins and the perpetual stream silently never subscribes.
    // Refusing here turns that into a startup error instead of a gateway that
    // runs, looks healthy, and delivers half of what was asked for.
    if (config.enable_spot && config.enable_perp) {
        return std::unexpected{MakeError::TwoMarketsAtOnce};
    }
    if (!config.enable_spot && !config.enable_perp) {
        return std::unexpected{MakeError::NoMarketSelected};
    }
    if (caps.max_client_order_id_len < k_min_client_order_id_len) {
        return std::unexpected{MakeError::ClientOrderIdTooShort};
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
    case MakeError::TwoMarketsAtOnce:
        return "one adapter serves one market; run a second process";
    case MakeError::NoMarketSelected:
        return "no market selected";
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
