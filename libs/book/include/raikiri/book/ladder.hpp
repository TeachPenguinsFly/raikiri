#pragma once

// The price ladder every venue's order book is built on.
//
// Ten adapters wrote this independently before it was worth admitting it is
// one thing. What they genuinely do NOT share is the continuity state machine
// — the token comes in five shapes across the sixteen venues (successor,
// back-pointer, range, id-keyed, and none at all) and no two agree on how a
// snapshot announces itself. So the ladder takes the venue's verdict as an
// input and never computes one: `apply` here means "these levels are known
// good", and deciding that stays where the venue's rules are understood.

#include <raikiri/domain/market_data.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace raikiri::book {

struct Level {
    PriceTicks price{};
    QtyLots qty{};
};

enum class Side : std::uint8_t { Bid, Ask };

// A single-sided price ladder. Bids descend, asks ascend, and both stay sorted
// so the touch is always element zero.
class Ladder {
public:
    explicit Ladder(Side side) noexcept : side_{side} {}

    // Replaces the level at that price, or inserts it. A zero quantity is NOT
    // special here: venues disagree on whether a delete is a zero size, an
    // explicit action, or a zero order count, so the caller decides and calls
    // remove() instead.
    void upsert(Level level);

    void remove(PriceTicks price);

    void clear() noexcept { levels_.clear(); }

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

    [[nodiscard]] std::span<const Level> levels() const noexcept { return levels_; }

    // Drops everything past `depth`, keeping the touch. Venues that push the
    // whole book need this; venues that maintain a fixed window do not.
    void truncate(std::size_t depth);

    // Everything worse than this price, inclusive of nothing at the touch —
    // the pruning a seeded book needs when the snapshot only vouched for a
    // band and the stream kept widening it.
    void drop_beyond(PriceTicks worst);

private:
    [[nodiscard]] bool precedes(PriceTicks a, PriceTicks b) const noexcept {
        return side_ == Side::Bid ? a.raw > b.raw : a.raw < b.raw;
    }

    Side side_;
    std::vector<Level> levels_;
};

// Both sides plus the publication rule they share: a book that is one-sided is
// not a book, and a consumer cannot tell a stale one from a live one, so a
// book we no longer trust is published as nothing at all.
class Ladders {
public:
    Ladders() : bids_{Side::Bid}, asks_{Side::Ask} {}

    [[nodiscard]] Ladder& bids() noexcept { return bids_; }

    [[nodiscard]] Ladder& asks() noexcept { return asks_; }

    [[nodiscard]] const Ladder& bids() const noexcept { return bids_; }

    [[nodiscard]] const Ladder& asks() const noexcept { return asks_; }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
    }

    // The top k_book_levels a side. Nullopt while either side is empty.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(InstrumentId instrument,
                                                       std::uint64_t venue_seq,
                                                       UnixNanos ts_event,
                                                       UnixNanos ts_init) const;

private:
    Ladder bids_;
    Ladder asks_;
};

// Exact conversion against the instrument's own step. A venue value that does
// not divide evenly means the venue changed a filter, so the caller drops the
// message rather than rounding — for a DELTA book it must invalidate the whole
// book, because one skipped level makes our copy diverge from the venue's
// silently and permanently.
[[nodiscard]] std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step);

} // namespace raikiri::book
