#pragma once

// Order book maintenance for the `level2_batch` channel.
//
// Two things decide the shape of this class, both measured on BTC-USD
// 2026-08-17 rather than read off the docs:
//
//   * the opening snapshot is the WHOLE book — 18,427 bids and 27,378 asks in
//     one 1,203,894-byte frame — which is why the venue raises the transport's
//     max_message_bytes and why the parser stops at k_max_levels below.
//     What is kept is a band, and a later change outside it is dropped rather
//     than believed: nothing confirms a level the snapshot never showed, and
//     carrying it would make the book look deeper than it is known to be.
//     binance's seeded book bands itself for the same reason.
//   * there is NO sequence anywhere on this channel. A change frame carries
//     type, product_id, changes and time, so a dropped frame leaves no
//     arithmetic trace — the same silence bitfinex's book channel has, and
//     unlike BitMEX there are not even row ids to make a lost insert visible.
//     Coinbase's sequence numbers live on the `full` channel, which is a
//     different subscription carrying a different payload.
//
// A change is ["buy"|"sell", price, size] with an ABSOLUTE size; zero deletes.
//
// The ladder underneath is raikiri::book's. What is not shared with another
// venue is the banding above it, and what leaves the class is a snapshot the
// gateway can publish without the consumer knowing any of it.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// level2_batch levels are ["price","size"] pairs of decimal STRINGS. A size of
// zero deletes the level; it appears only on the updates.
struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// THE OPENING SNAPSHOT IS THE WHOLE BOOK. Measured on BTC-USD 2026-08-17:
// 18,427 bids and 27,378 asks in one 1,203,894-byte frame, which is why this
// venue raises the transport's max_message_bytes.
//
// Both arrays arrive sorted best-first, so reading stops at k_max_levels
// rather than failing: what is dropped is the far tail of a book nothing
// downstream carries. The book class bands itself at the last level kept, so
// a later change out there is dropped rather than believed.
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireL2Snapshot {
    std::string_view product_id;
    std::string_view time; // ISO-8601; absent on the snapshot in some captures
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireL2Snapshot, WireError> parse_l2_snapshot(std::string_view frame,
                                                                         LevelBuffer& storage);

// A change is ["buy"|"sell","price","size"] — the SIDE is a word here, not the
// sign of the size the way bitfinex writes it, and the size is absolute.
struct WireChange {
    std::string_view price;
    std::string_view qty;
    bool bid{false};
};

// The largest observed batch is a few dozen; the ceiling leaves room without
// letting a frame make us allocate.
inline constexpr std::size_t k_max_changes = 4096;

struct ChangeBuffer {
    std::array<WireChange, k_max_changes> changes{};
};

// There is NO sequence on this channel — a frame carries type, product_id,
// changes and time, and nothing else.
struct WireL2Update {
    std::string_view product_id;
    std::string_view time; // ISO-8601
    std::span<const WireChange> changes;
};

[[nodiscard]] std::expected<WireL2Update, WireError> parse_l2_update(std::string_view frame,
                                                                     ChangeBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // changes applied onto the book we hold
    Resynced, // the opening snapshot replaced the book
    Ignored,  // a change arriving before any snapshot
    BadLevel, // a price or size failed exact fixed-point conversion
};

// One product's book. Venue strings become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from Coinbase.
class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    BookResult reset(const WireL2Snapshot& snapshot);
    BookResult apply(const WireL2Update& update);

    // The top k_book_levels a side, or nullopt while the book is one-sided or
    // out of sync. A consumer cannot tell a stale book from a live one, so a
    // book we no longer trust is published as nothing at all.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    // Changes at prices the snapshot never vouched for. Not an error — the
    // book is banded on purpose — but a count worth watching.
    [[nodiscard]] std::uint64_t outside_band() const noexcept { return outside_band_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] std::optional<Level> convert(const WireLevel& wire) const;
    static void upsert(book::Ladder& side, const Level& level);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    PriceTicks band_low_{};  // the deepest bid the snapshot vouched for
    PriceTicks band_high_{}; // and the highest ask
    std::uint64_t outside_band_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::coinbase
