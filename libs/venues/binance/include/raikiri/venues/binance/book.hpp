#pragma once

// Order book maintenance for binance USD-M futures.
//
// <sym>@depth@100ms carries deltas and no snapshot, so the book is seeded from
// GET /fapi/v1/depth and then chained. Three rules, all verified against the
// live stream rather than taken from the docs:
//
//   * the futures chain is `pu == the previous event's u`. The spot rule,
//     `U == previous u + 1`, held on 0 of 105 consecutive frames — it would
//     reject every valid futures delta.
//   * the seed is a photograph of one instant, usable only while some delta
//     still straddles it (U <= lastUpdateId <= u). Ids are not dense per
//     symbol — consecutive frames left holes of ~100 ids — so a seed nothing
//     straddles is discarded rather than trusted.
//   * the stream is full-depth and unbounded: a book seeded with 1000 levels
//     a side grew past 1400 in seventeen seconds. Levels outside the band the
//     seed vouched for are dropped, because nothing else can confirm them.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/binance/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace raikiri::venues::binance {

enum class BookResult : std::uint8_t {
    Applied,  // book advanced and is publishable
    Buffered, // held until the seed lands
    Stale,    // wholly before the seed, or already applied
    Waiting,  // seeded, waiting for the delta that straddles the seed
    Gap,      // the pu chain broke: book dropped, re-seed required
    Reseed,   // the seed can no longer be bridged: fetch another
};

// Deltas held while the REST seed is in flight. One round trip at a 100 ms
// cadence is a handful of frames; this is sized for a very slow one, and an
// overflow restarts the buffer rather than growing without bound.
inline constexpr std::size_t k_max_buffered_deltas = 64;

// The seed depth, and so the width of the band the book is trusted inside.
inline constexpr std::uint32_t k_seed_depth = 1000;

// One symbol's book. Prices and sizes become instrument-scaled integers at
// ingest, so nothing downstream ever sees a venue string.
class DepthBook {
public:
    struct Level {
        PriceTicks price{};
        QtyLots qty{};
    };

    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    // True while the book cannot publish and no snapshot is outstanding.
    [[nodiscard]] bool wants_seed() const noexcept;

    void seed_requested() noexcept { seed_in_flight_ = true; }

    void seed_failed() noexcept { seed_in_flight_ = false; }

    BookResult seed(const WireDepthSnapshot& snapshot);
    BookResult apply(const WireDepthUpdate& delta);

    [[nodiscard]] bool live() const noexcept { return state_ == State::Live; }

    // The book as shared memory takes it, or nullopt while it is unseeded or
    // desynchronised.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::uint64_t reseeds() const noexcept { return reseeds_; }

    [[nodiscard]] std::uint64_t dropped_levels() const noexcept { return dropped_levels_; }

    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_id_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return bids_; }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return asks_; }

private:
    enum class State : std::uint8_t {
        Cold,     // no usable seed; deltas are buffered
        Bridging, // seed in hand, waiting for the delta that straddles it
        Live,
    };

    struct Buffered {
        std::uint64_t first_id{0};
        std::uint64_t last_id{0};
        std::uint64_t prev_id{0};
        std::vector<Level> bids;
        std::vector<Level> asks;
    };

    [[nodiscard]] std::optional<Level> convert(const WireLevel& wire) const;
    static void upsert(std::vector<Level>& side, bool descending, const Level& level);

    void apply_wire(std::span<const WireLevel> levels, std::vector<Level>& side, bool descending);
    static void
    apply_levels(std::span<const Level> levels, std::vector<Level>& side, bool descending);

    BookResult buffer(const WireDepthUpdate& delta);
    BookResult bridge(const WireDepthUpdate& delta);
    BookResult advance(const WireDepthUpdate& delta);
    BookResult replay();
    BookResult settle();

    void install_held();
    void discard_held();

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    std::vector<Level> bids_; // descending by price
    std::vector<Level> asks_; // ascending by price
    std::vector<Buffered> pending_;
    std::vector<Level> held_bids_;
    std::vector<Level> held_asks_;
    std::uint64_t held_id_{0};
    PriceTicks band_low_{};  // worst bid the seed vouched for
    PriceTicks band_high_{}; // worst ask the seed vouched for
    std::uint64_t last_id_{0};
    std::uint64_t gaps_{0};
    std::uint64_t reseeds_{0};
    std::uint64_t dropped_levels_{0};
    State state_{State::Cold};
    bool seed_in_flight_{false};
};

} // namespace raikiri::venues::binance
