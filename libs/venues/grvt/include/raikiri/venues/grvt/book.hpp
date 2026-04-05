#pragma once

// What a reader must hold to keep a GRVT delta stream in step: the book and
// the top-of-book quote, and the sequence rule both of them obey.
//
// THE RULE, verified against the live venue on 2026-09-24 rather than taken
// from the docs:
//
//   * subscribing answers with an ack carrying `latest_sequence_number`, then
//     a snapshot whose own sequence is 0, then deltas;
//   * the first delta's `prev_sequence_number` IS that latest number, and
//     every later delta's `prev` is the sequence of the one before — per
//     selector, not per stream: 13 662 consecutive quote deltas and 2876 book
//     deltas across the catalogue chained without a hole;
//   * subscribing the same selector again answers a fresh ack and a fresh
//     snapshot on the same socket, which is how a gap is repaired — no REST
//     seed exists or is needed.
//
// So the ack is what anchors a snapshot, and a delta that does not chain is
// never applied: the book goes cold and asks again.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/grvt/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::grvt {

enum class Step : std::uint8_t {
    Snapshot, // start over from this frame
    Apply,    // chains onto the last frame
    Stale,    // already applied, or from before the snapshot
    Cold,     // no snapshot in hand; nothing to apply it to
    Gap,      // the chain broke: the state is dropped and must be asked for again
};

class Sequence {
public:
    // The ack names what the next snapshot describes.
    void acknowledged(std::uint64_t latest) noexcept {
        anchor_ = latest;
        anchored_ = true;
    }

    [[nodiscard]] Step offer(std::uint64_t sequence, std::uint64_t prev) noexcept;

    [[nodiscard]] bool live() const noexcept { return live_; }

    [[nodiscard]] std::uint64_t last() const noexcept { return last_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    // Ask once per break. The flag clears when a snapshot lands, so a gap
    // that repeats before the answer arrives does not ask twice.
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

// One instrument's ladder, full depth as the venue keeps it. Prices and sizes
// become the instrument's ticks and lots at ingest; one that will not convert
// exactly drops the whole book, because a skipped level makes this copy
// diverge from the venue's silently and for good.
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

// The top of book as v1.mini.d leaves it after every delta. Each field is
// UNDEF until the venue states it and again when the venue nulls it.
class Quote {
public:
    Quote(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    // False when a stated value will not convert; the quote is then cleared,
    // since a merged state with one field missing is a quote nobody sent.
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
