#pragma once

// Static capability descriptor per venue. Capabilities are DATA, not code:
// the generic gateway consults the descriptor (can this venue batch-cancel?
// does it push a book checksum? how long may a client order id be?) instead
// of growing per-venue if-ladders. Adding a venue = one adapter + one row.
//
// Values marked provisional are compiled-in knowledge pending the live
// docs-research pass; the ULID static_assert below is the load-bearing one.

#include <raikiri/domain/identifiers.hpp>

#include <cstdint>

namespace raikiri::exec {

// How the private/user-data stream is established.
enum class UserStreamModel : std::uint8_t {
    ListenKey,     // REST-minted key + periodic keepalive (binance family)
    AuthSubscribe, // sign/login over the socket itself (bybit/okx family)
    None,
};

struct VenueCaps {
    VenueId venue{VenueId::Invalid};
    bool spot{false};
    bool perp{false};

    bool ws_order_entry{false};  // orders placeable over a WS trade API
    bool fix_order_entry{false}; // FIX offered at any access tier
    bool cancel_by_client_order_id{false};
    bool batch_cancel{false};
    bool amend_order{false};       // native modify (vs cancel+replace)
    bool native_post_only{false};  // venue-enforced maker-only
    bool book_checksum{false};     // venue pushes book checksums (okx-style)

    UserStreamModel user_stream{UserStreamModel::None};
    std::uint8_t max_client_order_id_len{0};
    std::uint32_t headline_order_rate_per_s{0}; // informative, not a limiter
};

// Compiled capability rows for the supported venues (+ sim).
[[nodiscard]] const VenueCaps& caps_of(VenueId venue) noexcept;

// The OMS mints 26-char ULIDs as exchange client order ids; a venue that
// cannot carry 26 chars breaks the recovery story and must be rejected at
// registration, not discovered in production.
inline constexpr std::uint8_t k_min_client_order_id_len = 26;

} // namespace raikiri::exec
