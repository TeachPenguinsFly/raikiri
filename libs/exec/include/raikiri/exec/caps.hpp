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
    // The markets this ADAPTER serves, which is not the same as the markets
    // the venue lists: a row says true only where the code has been written
    // and run. Dated futures and options default false because almost nothing
    // implements them, and a row that claimed one would be accepted at load
    // and then fail one call at a time.
    bool spot{false};
    bool perp{false};
    bool future{false};
    bool option{false};

    [[nodiscard]] constexpr bool serves(MarketType market) const noexcept {
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

    bool ws_order_entry{false};  // orders placeable over a WS trade API
    bool fix_order_entry{false}; // FIX offered at any access tier
    bool cancel_by_client_order_id{false};
    bool batch_cancel{false};
    bool amend_order{false};      // native modify (vs cancel+replace)
    bool native_post_only{false}; // venue-enforced maker-only
    bool book_checksum{false};    // venue pushes book checksums (okx-style)

    // Whether the reconciliation pulls are implemented. Default false, and the
    // reconciler consults this BEFORE the call: without it, an adapter that
    // answers "no positions" and an adapter that cannot answer at all look
    // identical, and the reconciler reads the second as the first.
    bool position_pull{false};
    // Whether the adapter can list what the venue still has WORKING. Same rule
    // as position_pull, and the stakes are higher: "no open orders" read off a
    // venue that cannot answer would close a desk's whole book.
    bool order_pull{false};
    bool fill_pull{false};
    // Whether the adapter reads what the account holds. Same rule as the two
    // above: absence of a balance and an inability to ask are different facts,
    // and a margin model built on the wrong one is built on nothing.
    bool balance_pull{false};
    // Whether the adapter reads the venue's own record of what funding it
    // charged. Same rule again, and it matters most here: funding is the one
    // number a caller is tempted to reconstruct from the public rate, and a
    // reconstruction disagrees with the venue's books in an append-only
    // ledger. False means this venue contributes no funding PnL, which is the
    // honest state rather than a wrong one.
    bool funding_charge{false};

    // The venue publishes a rolling 24-hour turnover on its ticker AND this
    // adapter reads it. False means the board has no volume for this venue —
    // which must not be drawn as a zero.
    bool ticker_stats{false};

    // A third credential beyond key and secret. The adapters that need one
    // refuse to sign without it, and two of them refuse quietly — okx and
    // bitget start successfully and then reject every order locally — so the
    // process has to know before it accepts a command that it cannot sign.
    bool needs_passphrase{false};

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
