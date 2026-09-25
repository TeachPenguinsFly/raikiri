#pragma once

#include <raikiri/domain/identifiers.hpp>

namespace raikiri::gatewayd {

// The sim venue signs nothing, so it is the one account read without keys.
[[nodiscard]] constexpr bool
private_pulls_enabled(VenueId venue, bool can_trade, bool credentials_configured) noexcept {
    return can_trade && (credentials_configured || venue == VenueId::Sim);
}

} // namespace raikiri::gatewayd
