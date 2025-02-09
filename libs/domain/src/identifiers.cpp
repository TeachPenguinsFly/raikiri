#include <raikiri/domain/identifiers.hpp>

#include <array>
#include <cstring>

namespace raikiri {

namespace {

constexpr std::array<std::string_view, k_venue_count> k_venue_names{
    "invalid", "binance", "bybit", "okx", "gate", "kucoin", "bingx", "xt", "mexc", "sim",
};

} // namespace

std::string_view to_string(VenueId venue) noexcept {
    const auto idx = static_cast<std::size_t>(venue);
    return idx < k_venue_names.size() ? k_venue_names[idx] : k_venue_names[0];
}

VenueId venue_from_string(std::string_view name) noexcept {
    for (std::size_t i = 1; i < k_venue_names.size(); ++i) {
        if (k_venue_names[i] == name) {
            return static_cast<VenueId>(i);
        }
    }
    return VenueId::Invalid;
}

std::string_view to_string(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return "spot";
    case MarketType::Perp:
        return "perp";
    case MarketType::Future:
        return "future";
    case MarketType::Option:
        return "option";
    }
    return "unknown";
}

std::string_view to_string(Side side) noexcept {
    return side == Side::Buy ? "buy" : "sell";
}

std::string_view format_account(AccountId account, char* buf, std::size_t buf_len) noexcept {
    const auto venue = to_string(account.venue);
    // Perp is the unsuffixed default for Go-system compatibility.
    std::string_view suffix;
    switch (account.market) {
    case MarketType::Perp:
        suffix = "";
        break;
    case MarketType::Spot:
        suffix = "-spot";
        break;
    case MarketType::Future:
        suffix = "-fut";
        break;
    case MarketType::Option:
        suffix = "-opt";
        break;
    }
    if (buf_len < venue.size() + suffix.size()) {
        return {};
    }
    std::memcpy(buf, venue.data(), venue.size());
    std::memcpy(buf + venue.size(), suffix.data(), suffix.size());
    return {buf, venue.size() + suffix.size()};
}

} // namespace raikiri
