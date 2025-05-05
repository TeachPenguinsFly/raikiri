#pragma once

// Order book maintenance for the `books` channel.
//
// OKX chains its frames on seqId: a delta belongs after the message whose
// seqId equals its prevSeqId, and a snapshot carries prevSeqId -1. That chain
// is the only continuity guarantee here — the checksum field was deprecated on
// 2026-06-23 and is pinned to 0, so it is not surfaced at all (see wire.hpp).
//
// Nothing about this book is shared with another venue's: the sequencing rules
// are OKX's, and what leaves the class is a snapshot the gateway can publish
// without the consumer knowing any of them.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/okx/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace raikiri::venues::okx {

enum class BookResult : std::uint8_t {
    Applied,     // delta chained onto the book we hold
    Resynced,    // snapshot replaced the book
    GapDetected, // the seqId chain broke: book dropped until the next snapshot
    Ignored,     // a frame the book already contains, or one with no book yet
    BadLevel,    // a price or size failed exact fixed-point conversion
};

// One instrument's book. Venue strings become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from OKX.
class Book {
public:
    struct Level {
        PriceTicks price{};
        QtyLots qty{};
    };

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBooks& update);

    // The top k_book_levels a side, or nullopt while the book is one-sided or
    // out of sync. A consumer cannot tell a stale book from a live one, so a
    // book we no longer trust is published as nothing at all.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::int64_t last_seq_id() const noexcept { return last_seq_id_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return bids_; }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return asks_; }

private:
    [[nodiscard]] bool
    apply_side(std::span<const WireLevel> levels, std::vector<Level>& side, bool descending);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    std::vector<Level> bids_; // descending by price
    std::vector<Level> asks_; // ascending by price
    std::int64_t last_seq_id_{-1};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::okx
