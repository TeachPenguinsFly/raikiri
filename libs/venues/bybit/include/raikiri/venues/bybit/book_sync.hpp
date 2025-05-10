#pragma once

// Order book synchronisation for bybit v5.
//
// Deliberately NOT here: checksum validation. OKX has one (deprecated as of
// 2026-06), bybit never did — the update-id chain is the guarantee, and
// inventing a checksum over a book the venue never checksummed would only
// produce false alarms.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bybit/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::venues::bybit {

enum class SyncResult : std::uint8_t {
    Applied,     // book updated and coherent
    Resynced,    // snapshot replaced the book
    GapDetected, // update id skipped: book dropped, resubscribe required
    Ignored,     // stale message (update id already applied)
    BadLevel,    // a price or size failed exact fixed-point conversion
};

// One symbol's book. Prices and sizes are converted to instrument-scaled
// integers at ingest, so nothing downstream ever sees a venue string.
class BookSync {
public:
    using Level = book::Level;

    BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] SyncResult apply(const WireOrderbook& update);

    // Top of book, or nullopt while the book is empty or desynchronised.
    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

    // The top k_book_levels a side, under the same rule as bbo(): nothing at
    // all while desynchronised, because a stale book is priced off as if it
    // were current while silence is visibly missing data.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_update_id_; }

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
    std::uint64_t last_update_id_{0};
    std::uint64_t last_seq_{0};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::bybit
