#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/grvt/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::grvt {

enum class Step : std::uint8_t {
    Snapshot,
    Apply,
    Stale, // already applied, or from before the snapshot
    Cold,
    Gap,   // the chain broke: the state is dropped and asked for again
};

// The ack's latest_sequence_number anchors the snapshot; a delta whose prev is
// not the last sequence is never applied, and the book resubscribes.
class Sequence {
public:
    void acknowledged(std::uint64_t latest) noexcept {
        anchor_ = latest;
        anchored_ = true;
    }

    [[nodiscard]] Step offer(std::uint64_t sequence, std::uint64_t prev) noexcept;

    [[nodiscard]] bool live() const noexcept { return live_; }

    [[nodiscard]] std::uint64_t last() const noexcept { return last_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    // True once per break; clears when a snapshot lands.
    [[nodiscard]] bool take_resubscribe() noexcept;

    void drop() noexcept;

private:
    std::uint64_t anchor_{0};
    std::uint64_t last_{0};
    std::uint64_t gaps_{0};
    bool anchored_{false};
    bool live_{false};
    bool asked_{false};
};

// A level that will not convert exactly drops the whole book: skipping it would
// diverge from the venue silently.
class DepthBook {
public:
    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] bool replace(const WireBook& snapshot);
    [[nodiscard]] bool apply(const WireBook& delta);

    void clear() noexcept { ladders_.clear(); }

    [[nodiscard]] std::optional<BookSnapshot>
    snapshot(std::uint64_t venue_seq, UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] const book::Ladders& ladders() const noexcept { return ladders_; }

    Sequence sequence;

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
};

// Fields are UNDEF until stated and again when the venue nulls them.
class Quote {
public:
    Quote(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    // False on a value that will not convert; the quote is then cleared.
    [[nodiscard]] bool replace(const WireQuote& snapshot);
    [[nodiscard]] bool apply(const WireQuote& delta);

    void clear() noexcept;

    // Nullopt until all four fields are known.
    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const noexcept;

    Sequence sequence;

private:
    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    PriceTicks bid_{};
    PriceTicks ask_{};
    QtyLots bid_qty_{};
    QtyLots ask_qty_{};
};

} // namespace raikiri::venues::grvt
