#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/dydx/wire.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::venues::dydx {

// The subscribe snapshot stops at this many levels a side while the live book
// runs deeper, so a full side is a band: changes beyond its last level are dropped.
inline constexpr std::size_t k_snapshot_depth = 100;

enum class BookResult : std::uint8_t {
    Resynced,
    Applied,
    Ignored, // no snapshot under it
    Crossed, // applied, but bid >= ask: nothing is published until it clears or is re-asked
    Lost,    // a level off the grid, or a banded side emptied: the book is dropped
};

class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult reset(std::span<const WireLevel> bids,
                                   std::span<const WireLevel> asks);
    [[nodiscard]] BookResult apply(std::span<const WireLevel> bids,
                                   std::span<const WireLevel> asks);

    // Nullopt while unsynced, crossed or one-sided.
    [[nodiscard]] std::optional<Bbo> top(UnixNanos ts_event, UnixNanos ts_init) const;
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    // For a socket that reopened: its resubscription brings the snapshot.
    void invalidate() noexcept;

    // Drops the book and asks for it again through take_resync.
    void drop() noexcept;

    // True at most once per cooldown after a drop or a crossing: a third
    // subscribe to one ticker inside a second closes the whole socket.
    [[nodiscard]] bool take_resync(std::uint64_t now_ns) noexcept;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] bool crossed() const noexcept { return crossed_; }

    [[nodiscard]] std::uint64_t outside_band() const noexcept { return outside_band_; }

    [[nodiscard]] std::uint64_t crossings() const noexcept { return crossings_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, bool bid);
    [[nodiscard]] BookResult settle();

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::optional<PriceTicks> bid_band_;
    std::optional<PriceTicks> ask_band_;
    std::uint64_t outside_band_{0};
    std::uint64_t crossings_{0};
    std::uint64_t next_resync_ns_{0};
    bool synced_{false};
    bool crossed_{false};
    bool resync_wanted_{false};
};

inline constexpr std::uint64_t k_resync_cooldown_ns = 2ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::dydx
