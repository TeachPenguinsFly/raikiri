#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/paradex/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::paradex {

enum class BookResult : std::uint8_t {
    Resynced,
    Applied,
    Ignored, // no snapshot under it, or a delta the book already holds
    Crossed, // applied, but bid >= ask: nothing is published until it clears
    Lost,    // a sequence gap or a level off the grid: the book is dropped
};

// the first frame of a subscription is the whole api book; every delta after it is seq + 1, empty ones too
class Book {
public:
    explicit Book(const InstrumentBase& base);

    [[nodiscard]] BookResult on_frame(const WireBook& wire);

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] std::optional<Bbo> top(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] bool take_resync(std::uint64_t now_ns) noexcept;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::int64_t seq() const noexcept { return seq_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, bool bid);
    [[nodiscard]] BookResult lose() noexcept;

    InstrumentBase base_{};
    book::Ladders ladders_;
    std::int64_t seq_{0};
    std::uint64_t gaps_{0};
    std::uint64_t next_resync_ns_{0};
    bool synced_{false};
    bool crossed_{false};
    bool resync_wanted_{false};
};

inline constexpr std::uint64_t k_resync_cooldown_ns = 2ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::paradex
