#pragma once

// static capability row per venue

#include <raikiri/domain/identifiers.hpp>

#include <cstdint>

namespace raikiri::exec {

// how the private user-data stream is established
enum class UserStreamModel : std::uint8_t {
    ListenKey,     // REST-minted key + periodic keepalive (binance family)
    AuthSubscribe, // sign/login over the socket itself (bybit/okx family)
    None,
};

// per-market capability set; empty by default
struct MarketSet {
    bool spot{false};
    bool perp{false};
    bool future{false};
    bool option{false};

    [[nodiscard]] constexpr bool has(MarketType market) const noexcept {
        switch (market) {
        case MarketType::Spot:
            return spot;
        case MarketType::Perp:
            return perp;
        case MarketType::Future:
            return future;
        case MarketType::Option:
            return option;
        }
        return false;
    }

    [[nodiscard]] constexpr bool any() const noexcept { return spot || perp || future || option; }
};

struct VenueCaps {
    VenueId venue{VenueId::Invalid};
    // markets this adapter implements, which may be fewer than the venue lists
    MarketSet markets{};

    [[nodiscard]] constexpr bool serves(MarketType market) const noexcept {
        return markets.has(market);
    }

    bool ws_order_entry{false};  // orders placeable over a WS trade API
    bool fix_order_entry{false}; // FIX offered at any access tier
    bool cancel_by_client_order_id{false};
    bool batch_cancel{false};
    bool amend_order{false};      // native modify (vs cancel+replace)
    bool native_post_only{false}; // venue-enforced maker-only
    bool book_checksum{false};    // venue pushes book checksums (okx-style)

    // markets the position pull is implemented for; absence means the adapter cannot ask
    MarketSet position_pull{};
    // markets where open orders can be pulled; same rule as position_pull
    MarketSet order_pull{};
    MarketSet fill_pull{};
    // markets where balances can be pulled; same rule as position_pull
    MarketSet balance_pull{};
    // markets where the venue's funding charges can be read; empty means no funding pnl
    MarketSet funding_charge{};

    // the adapter reads the venue's 24 h turnover; false means no volume, not zero
    bool ticker_stats{false};

    // okx and bitget start without one and then reject every order locally
    bool needs_passphrase{false};

    UserStreamModel user_stream{UserStreamModel::None};
    std::uint8_t max_client_order_id_len{0};
    std::uint32_t headline_order_rate_per_s{0}; // informative, not a limiter
};

// compiled capability rows for the supported venues and sim
[[nodiscard]] const VenueCaps& caps_of(VenueId venue) noexcept;

// the oms mints 26-char ulids as exchange client order ids
inline constexpr std::uint8_t k_min_client_order_id_len = 26;

} // namespace raikiri::exec
