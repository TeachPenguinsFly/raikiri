#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/apex/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::venues::apex {

enum class SyncResult : std::uint8_t {
    Applied,
    Resynced,    // a snapshot replaced the book
    GapDetected, // `u` skipped: book dropped until the next snapshot
    Ignored,     // a delta with no book under it, or one already applied
    BadLevel,    // a price or size off the instrument's grid: book dropped
};

// A snapshot's `u` continues the REST sequence rather than restarting at 1, as
// the docs claim; it seeds the chain and each delta must be exactly one past it.
class BookSync {
public:
    using Level = book::Level;

    BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] SyncResult apply(const WireOrderbook& update);

    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

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
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::apex
