#pragma once

// Single-instrument limit order book for the sim venue (VenueId::Sim).
//
// Price-time priority: levels ordered by price (bids descending, asks
// ascending), FIFO within a level. std::map + std::deque on purpose — the
// sim is a correctness tool, not the hot path, and tree maps keep level
// iteration trivially ordered and debuggable.
//
// Fills always execute at the RESTING order's price: the aggressor pays what
// the book displays, exactly as on a real venue. Determinism: the book has
// no clock and no randomness — state depends only on the call sequence.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace raikiri::simcore {

// Owner tag 0 marks background ("scenery") liquidity; any other value is a
// client order the Engine tracks in its registry.
inline constexpr std::uint32_t k_background_owner = 0;

// What happened to the taker's remainder after matching.
enum class Disposition : std::uint8_t {
    Filled,           // fully filled, nothing left over
    Rested,           // GTC remainder joined the book
    Expired,          // IOC/FOK/market remainder discarded — never rests
    RejectedPostOnly, // post-only would have crossed; book untouched, no fills
};

enum class BookError : std::uint8_t {
    UnknownOrder,
    BadQty,
};

// One maker<->taker execution. maker_leaves rides along so the Engine can
// build the maker-side Filled event without re-querying the book.
struct Fill {
    std::uint64_t maker_id{0};
    std::uint32_t maker_owner{0};
    PriceTicks price{}; // resting order's price
    QtyLots qty{};
    QtyLots maker_leaves{}; // maker remainder after this fill
};

struct MatchResult {
    std::vector<Fill> fills;
    Disposition disposition{Disposition::Rested};
    QtyLots remainder{}; // taker leaves after the operation
};

class Book {
public:
    struct RestingOrder {
        std::uint64_t id{0};    // sim-minted exchange order id
        std::uint32_t owner{0}; // k_background_owner or a client tag
        Side side{Side::Buy};
        PriceTicks price{};
        QtyLots leaves{};
        std::uint64_t seq{0}; // arrival sequence — the time-priority audit trail
    };

    struct BboLevels {
        PriceTicks bid{};
        PriceTicks ask{};
        QtyLots bid_qty{};
        QtyLots ask_qty{};

        friend bool operator==(const BboLevels&, const BboLevels&) noexcept = default;
    };

    [[nodiscard]] MatchResult submit_limit(std::uint64_t id,
                                           std::uint32_t owner,
                                           Side side,
                                           PriceTicks price,
                                           QtyLots qty,
                                           bool post_only,
                                           TimeInForce tif = TimeInForce::Gtc);

    [[nodiscard]] MatchResult
    submit_market(std::uint64_t id, std::uint32_t owner, Side side, QtyLots qty);

    bool cancel(std::uint64_t id);

    [[nodiscard]] std::expected<MatchResult, BookError>
    amend(std::uint64_t id, PriceTicks new_price, QtyLots new_leaves);

    [[nodiscard]] std::optional<BboLevels> bbo() const;

    [[nodiscard]] bool contains(std::uint64_t id) const noexcept { return index_.contains(id); }

    [[nodiscard]] const RestingOrder* find(std::uint64_t id) const noexcept;

private:
    using Level = std::deque<RestingOrder>;

    struct Locator {
        Side side{Side::Buy};
        PriceTicks price{};
    };

    std::map<PriceTicks, Level, std::greater<PriceTicks>> bids_; // best (highest) first
    std::map<PriceTicks, Level, std::less<PriceTicks>> asks_;    // best (lowest) first
    std::unordered_map<std::uint64_t, Locator> index_;           // id -> level, for O(1)-ish cancel
    std::uint64_t next_seq_{0};
};

} // namespace raikiri::simcore
