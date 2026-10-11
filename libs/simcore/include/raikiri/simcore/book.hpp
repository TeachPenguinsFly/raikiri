#pragma once

// price-time priority; fills execute at the resting order's price; no clock or randomness

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

// owner 0 is background liquidity; any other value is a client order
inline constexpr std::uint32_t k_background_owner = 0;

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

// maker_leaves lets the engine build the maker's Filled event without re-querying
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

    [[nodiscard]] MatchResult submit_market(std::uint64_t id, std::uint32_t owner, Side side, QtyLots qty);

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

    std::map<PriceTicks, Level, std::greater<>> bids_; // best (highest) first
    std::map<PriceTicks, Level, std::less<>> asks_;    // best (lowest) first
    std::unordered_map<std::uint64_t, Locator> index_; // id -> level, for O(1)-ish cancel
    std::uint64_t next_seq_{0};
};

} // namespace raikiri::simcore
