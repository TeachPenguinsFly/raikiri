#pragma once

// Order book maintenance for the `book` feed.
//
// Kraken chains on a single `seq` that steps by exactly one, snapshot
// included: VERIFIED on 8070 consecutive PF_XBTUSD frames, where the only
// observed gap between neighbours was 1 and the book_snapshot's seq was the
// predecessor of the first delta. There is no first/last id pair to straddle
// like binance's and no back-pointer like okx's prevSeqId — one number, one
// step, so a missed frame is arithmetic rather than a lookup.
//
// The other half of the shape: a delta is ONE level at the frame root with
// its own `side`, so every delta touches exactly one side and carries no
// batch. Kraken also pushes the whole book rather than a window — the live
// snapshot above held 1659 bids and 1288 asks — so nothing here prunes, and
// what shm gets is the top k_book_levels of a book that is much deeper.
//
// The ladder underneath is raikiri::book's. What is not shared with another
// venue is the sequencing, which is Kraken's, and what leaves the class is a
// snapshot the gateway can publish without the consumer knowing any of it.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // 0 means delete this level
};

// A live PF_XBTUSD `book_snapshot` carried 1751 bids and 1360 asks: Kraken
// pushes the WHOLE book, not a fixed depth, hence far above okx's 512.
inline constexpr std::size_t k_max_levels = 4096;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Snapshot and delta are different SHAPES, not one shape with a flag:
// `book_snapshot` carries bids/asks arrays of {price,qty}, a `book` delta is
// a single level at the root with its own `side` and fills exactly one span.
struct WireBook {
    std::string_view product_id;
    bool snapshot{false};
    std::uint64_t seq{0};
    std::uint64_t timestamp_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,     // delta chained onto the book we hold
    Resynced,    // book_snapshot replaced the book
    GapDetected, // seq skipped: book dropped until the next snapshot
    Ignored,     // a frame already applied, or a delta with no book yet
    BadLevel,    // a price or size failed exact fixed-point conversion
};

// One product's book. Venue tokens become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from Kraken.
class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBook& update);

    // The top k_book_levels a side, or nullopt while the book is one-sided or
    // out of sync. A consumer cannot tell a stale book from a live one, so a
    // book we no longer trust is published as nothing at all.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t last_seq_{0};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::kraken
