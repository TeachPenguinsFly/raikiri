#pragma once

// Order book maintenance for the `book` channel at P0/F0/len 100.
//
// This is the thinnest continuity guarantee of any venue here: Bitfinex sends
// NO sequence, NO previous sequence and no timestamp on a book frame. A data
// frame is [CHAN_ID, payload] and that is all of it, so a dropped frame leaves
// no arithmetic trace anywhere — not even BitMEX's id-keyed rows, which at
// least make a lost insert visible on the next update. What the venue does
// offer is a CHECKSUM conf flag (131072), which pushes [CHAN_ID,"cs",<crc32>]
// over the top 25 levels; it is additive rather than a change to the frames
// already parsed, and it is the one thing that would turn silence into a
// detectable divergence. Not implemented here — the CRC32 it needs is zlib's,
// not the CRC32C the repo already links — and recorded so the next person
// reaches for it rather than for a sequence that does not exist.
//
// Everything else was measured off the live socket on tBTCF0:USTF0, 866
// frames:
//
//   * a level is [PRICE, COUNT, AMOUNT], and AMOUNT's SIGN IS THE SIDE —
//     positive a bid, negative an ask. That is a different meaning from the
//     same field on the trades channel, where the sign is the aggressor.
//   * COUNT == 0 is the delete, and on a delete AMOUNT degenerates to +1/-1
//     and carries nothing but the side. Reading the quantity there would rest
//     a phantom unit at that price.
//   * the window is maintained by the venue: seeded at 100 a side, the book
//     never exceeded 100 and dipped only between a delete and the insert that
//     replaced it. Nothing here prunes, because there is nothing to prune.
//
// The ladder underneath is raikiri::book's. What is not shared with another
// venue is everything above, and what leaves the class is a snapshot the
// gateway can publish without the consumer knowing any of it.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bitfinex/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::venues::bitfinex {

enum class BookResult : std::uint8_t {
    Applied,  // levels applied onto the book we hold
    Resynced, // the opening array-of-arrays replaced the book
    Ignored,  // a delta arriving before any snapshot
    BadLevel, // a price or size failed exact fixed-point conversion
};

// One instrument's book. Venue tokens become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from Bitfinex.
class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBook& update);

    // The top k_book_levels a side, or nullopt while the book is one-sided or
    // out of sync. A consumer cannot tell a stale book from a live one, so a
    // book we no longer trust is published as nothing at all.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    // The same book's touch, for the BBO channel. This exists because the
    // `ticker` channel is not a quote stream on this venue: over one minute on
    // tBTCUSD it pushed 5 frames against this channel's 1261, both describing
    // the same top of book. Ticker still publishes; this is the fast source
    // beside it, not a replacement.
    [[nodiscard]] std::optional<Bbo> touch(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_level(const WireLevel& wire);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    bool synced_{false};
};

} // namespace raikiri::venues::bitfinex
