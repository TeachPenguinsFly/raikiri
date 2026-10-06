#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/arcus/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::arcus {

enum class BookResult : std::uint8_t {
    Resynced,
    Applied,
    Ignored, // no snapshot under it, or a delta the snapshot already holds
    Crossed, // applied, but bid >= ask: nothing is published until it clears
    Lost,    // a sequence gap or a level off the grid: the book is dropped
};

// the first delta may run a few numbers past the snapshot; after it they are contiguous
class Book {
public:
    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult reset(const WireBook& snapshot);
    [[nodiscard]] BookResult apply(const WireBook& delta);

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    // drops the book and asks for it again through take_resync
    void drop() noexcept;

    [[nodiscard]] bool take_resync(std::uint64_t now_ns) noexcept;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, bool bid);
    [[nodiscard]] BookResult settle();

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t seq_{0};
    std::uint64_t gaps_{0};
    std::uint64_t next_resync_ns_{0};
    bool synced_{false};
    bool streaming_{false};
    bool crossed_{false};
    bool resync_wanted_{false};
};

inline constexpr std::uint64_t k_resync_cooldown_ns = 5ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::arcus
