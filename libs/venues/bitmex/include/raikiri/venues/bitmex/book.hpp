#pragma once

// Order book maintenance for the `orderBookL2_25` table.
//
// BitMEX is the odd one of the delta venues: it has NO continuity token at
// all. A delta frame carries `table`, `action` and `data` and nothing else —
// no sequence, no previous sequence, no first/last pair — so there is no
// arithmetic that can tell a dropped frame from a quiet market. What replaces
// it is an id-keyed row set: the `partial` declares its own key as
// ["symbol","id","side"], and every later row names a level by that id rather
// than by price. The one divergence this DOES make visible is an `update`
// naming an id we do not hold, which can only mean a frame went missing, and
// that is what drops the book here.
//
// Two more facts, both measured on XBTUSD 2026-08-17 across 417 frames:
//
//   * BitMEX maintains the 25-level window itself, pairing each `insert` at
//     the touch with a `delete` of whatever fell off the far end. Nothing
//     here prunes, because there is nothing to prune — and 25 a side is the
//     deepest tier this venue offers below the 100 shm can hold.
//   * `update` carries only the columns that changed, so an absent `size` is
//     "unchanged", never zero. A `delete` carries no size at all.
//
// Nothing about this book is shared with another venue's: the row rules are
// BitMEX's, and what leaves the class is a snapshot the gateway can publish
// without the consumer knowing any of them.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bitmex/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace raikiri::venues::bitmex {

enum class BookResult : std::uint8_t {
    Applied,  // rows applied onto the book we hold
    Resynced, // a `partial` replaced the book
    Diverged, // an update named an id we do not hold: book dropped
    Ignored,  // rows arriving before any partial
    BadLevel, // a price or size failed exact fixed-point conversion
};

// One symbol's book. Venue tokens become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from BitMEX.
class Book {
public:
    struct Level {
        std::int64_t id{0};
        PriceTicks price{};
        QtyLots qty{};
    };

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(FrameAction action, std::span<const WireLevel> rows);

    // The top k_book_levels a side, or nullopt while the book is one-sided or
    // out of sync. A consumer cannot tell a stale book from a live one, so a
    // book we no longer trust is published as nothing at all.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t divergences() const noexcept { return divergences_; }

    // Deletes naming an id the book never held. Harmless to skip — removing
    // nothing removes nothing — but a rising count means frames are being lost.
    [[nodiscard]] std::uint64_t stale_deletes() const noexcept { return stale_deletes_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return bids_; }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return asks_; }

private:
    [[nodiscard]] std::vector<Level>& side_of(const WireLevel& row) noexcept;

    [[nodiscard]] bool upsert(const WireLevel& row);
    [[nodiscard]] BookResult amend(const WireLevel& row);
    void erase(const WireLevel& row);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    std::vector<Level> bids_; // descending by price
    std::vector<Level> asks_; // ascending by price
    std::uint64_t divergences_{0};
    std::uint64_t stale_deletes_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::bitmex
