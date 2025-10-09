#pragma once

// Order book maintenance for the `book.{instrument}.100ms` channel.
//
// Deribit chains on a BACK-POINTER, the same shape as OKX's prevSeqId: a
// delta belongs after the message whose change_id equals its prev_change_id.
// Two things separate it from OKX's anyway, both measured live rather than
// read off the docs:
//
//   * a snapshot carries no prev_change_id FIELD at all — not -1, not 0 — so
//     the snapshot flag comes from `type`, never from a sentinel value.
//   * change_id is not contiguous: consecutive frames on BTC-PERPETUAL jumped
//     by 305, 398, 403, 406 and 409. Nothing here may do arithmetic on it;
//     only the back-pointer says whether a frame fits.
//
// The other half of the shape: every level states its own action, so a
// removal is `delete` rather than a size. VERIFIED across 904 deletes on
// three perpetuals — every one of them also carried a size of 0.0, so the two
// readings agree today. The action is still what this book keys on, because
// the action is what Deribit defines and the zero is what it happens to send.
//
// The ladder underneath is raikiri::book's. What is not shared with another
// venue is the sequencing, which is Deribit's, and what leaves the class is a
// snapshot the gateway can publish without the consumer knowing any of it.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

struct WireBbo {
    std::string_view instrument;
    std::string_view bid_price; // raw JSON number tokens, exponents included
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::int64_t change_id{0};
    std::uint64_t ts_ms{0};
};

// Grouped, depth-limited books are always full snapshots — no type field, no
// prev_change_id — which is what makes this a BBO stream.
[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// Deribit states the ACTION per level instead of leaving a removal to be
// inferred from its quantity. MEASURED 2026-08-17 across 904 deletes on three
// perpetuals: every one ALSO carried 0.0, so the two readings happen to agree
// — but the action is what the venue defines, so it is what the book keys on.
enum class LevelAction : std::uint8_t { New, Change, Delete };

struct WireLevel {
    LevelAction action{LevelAction::New};
    std::string_view price;
    std::string_view qty;
};

// A live BTC-PERPETUAL snapshot ran 897 bids / 1080 asks. Past this a frame is
// REFUSED, not truncated — half a book is worse than no book.
inline constexpr std::size_t k_max_levels = 4096;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the change_id chain: a delta applies when its prev_change_id
// equals the previous message's change_id. A snapshot carries none at all.
struct WireBook {
    std::string_view instrument;
    bool snapshot{false};
    std::int64_t change_id{0};
    std::int64_t prev_change_id{0}; // 0 on a snapshot: the field is absent
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,     // delta chained onto the book we hold
    Resynced,    // a `snapshot` frame replaced the book
    GapDetected, // the change_id chain broke: book dropped until the next snapshot
    Ignored,     // a frame the book already contains, or one with no book yet
    BadLevel,    // a price or size failed exact fixed-point conversion
};

// One instrument's book. Venue tokens become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from Deribit.
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

    [[nodiscard]] std::int64_t last_change_id() const noexcept { return last_change_id_; }

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
    std::int64_t last_change_id_{-1};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::deribit
