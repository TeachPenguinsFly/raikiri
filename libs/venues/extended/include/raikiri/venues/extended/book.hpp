#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/extended/market_data.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::venues::extended {

enum class BookResult : std::uint8_t {
    Resynced, // a snapshot replaced the book
    Applied,
    Ignored,  // a delta with no snapshot under it
    BadLevel, // off the instrument's grid: book dropped until the next snapshot
};

// Chaining is the socket's business, not the book's: `seq` runs across every
// market on the stream, so a gap is detected there and invalidates them all.
class Book {
public:
    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBook& frame);

    void invalidate() noexcept;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::optional<Bbo> top(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] std::span<const book::Level> bids() const noexcept {
        return ladders_.bids().levels();
    }

    [[nodiscard]] std::span<const book::Level> asks() const noexcept {
        return ladders_.asks().levels();
    }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side) const;

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t seq_{0};
    bool synced_{false};
};

// The top of a depth=1 frame, which is a one-level snapshot. Nullopt when a
// side is empty or off the grid.
[[nodiscard]] std::optional<Bbo> map_top(const WireBook& frame,
                                         InstrumentId instrument,
                                         FixedE9 tick_size,
                                         FixedE9 lot_size,
                                         UnixNanos ts_init);

} // namespace raikiri::venues::extended
