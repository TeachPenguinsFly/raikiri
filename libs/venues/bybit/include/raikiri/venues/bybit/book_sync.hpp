#pragma once

// Order book synchronisation for bybit v5.
//
// Bybit sends a snapshot followed by deltas, each carrying an update id `u`
// that increments by exactly one per message for a given symbol. That single
// invariant is the whole protocol: a gap means we missed a delta and the book
// we hold is fiction. The only honest response is to drop it and wait for the
// venue to resend a snapshot (bybit pushes one automatically after a
// resubscribe), which is why a gap here surfaces as a resync request rather
// than a best-effort patch.
//
// Bybit v5 also ships a `seq` field for cross-stream ordering; it is
// recorded but not used for gap detection, because `u` is the documented
// per-symbol contract.
//
// Deliberately NOT here: checksum validation. OKX has one (deprecated as of
// 2026-06), bybit never did — the update-id chain is the guarantee, and
// inventing a checksum over a book the venue never checksummed would only
// produce false alarms.
//
// Depth is kept as a sorted array rather than a tree: the top of book is
// read every tick and the tail is touched rarely, so a flat vector with
// binary search beats pointer chasing at these depths (50 levels typical,
// 500 worst case).

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bybit/wire.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace raikiri::venues::bybit {

enum class SyncResult : std::uint8_t {
    Applied,      // book updated and coherent
    Resynced,     // snapshot replaced the book
    GapDetected,  // update id skipped: book dropped, resubscribe required
    Ignored,      // stale message (update id already applied)
    BadLevel,     // a price or size failed exact fixed-point conversion
};

// One symbol's book. Prices and sizes are converted to instrument-scaled
// integers at ingest, so nothing downstream ever sees a venue string.
class BookSync {
public:
    struct Level {
        PriceTicks price{};
        QtyLots size{};
    };

    // tick/lot come from the instrument table; a level that does not divide
    // exactly by them is a venue schema change, not something to round.
    BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] SyncResult apply(const WireOrderbook& update);

    // Top of book, or nullopt while the book is empty or desynchronised.
    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }
    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_update_id_; }
    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    // Called when the transport reconnects: the next message must be a
    // snapshot, and anything else is stale by definition.
    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return bids_; }
    [[nodiscard]] std::span<const Level> asks() const noexcept { return asks_; }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, std::vector<Level>& side,
                                  bool descending);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    std::vector<Level> bids_; // descending by price
    std::vector<Level> asks_; // ascending by price
    std::uint64_t last_update_id_{0};
    std::uint64_t last_seq_{0};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::bybit
