#pragma once

// The order book: what a frame says about it, and what a reader must hold to
// keep one in step.
//
// THE TWO BELONG TOGETHER. The wire type means nothing without the chaining
// rule that decides whether a delta may be applied at all, and the rule means
// nothing without the ids it reads.
//
// futures.order_book_update publishes deltas and never a snapshot, so the
// GET /futures/{settle}/order_book read is not an optimisation, it is the only
// way to start. Three things measured off the live socket, none of them taken
// from the docs:
//
//   * the chain is exactly contiguous: `U == the previous frame's u + 1` held
//     on 83 of 83 consecutive frames, with no id ever repeated or skipped.
//     There is no `pu` field and none is needed.
//   * the seed's `id` is the last update already in those levels, so the frame
//     that carries them forward is the one containing id + 1. Testing for `id`
//     instead would re-apply a frame at the boundary.
//   * the window is maintained by the venue. Subscribed at 100 levels and
//     seeded at 100, the book stayed at exactly 100 a side across every frame
//     applied — Gate deletes what falls out of the window. Nothing here
//     prunes, because there is nothing to prune.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::gate {

// Sizes are integer CONTRACT counts, so bid_qty/ask_qty are digit tokens with
// no decimal point. `ts_ms` is milliseconds here even though most other Gate
// epoch fields are seconds.
struct WireBookTicker {
    std::string_view contract;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t update_id{0}; // `u`, exceeds 2^32 in practice
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame);

// A level is {"p":"64156.4","s":35680} on both the websocket and the REST
// snapshot — same object either way, unlike okx's positional arrays.
struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the [first_update_id, last_update_id] chain: a delta applies
// when its first_update_id is one past the previous message's last_update_id.
// Gate publishes no snapshot on this channel, so seeding it requires the REST
// order_book read that rest.hpp builds.
struct WireBookUpdate {
    std::string_view contract;
    std::uint64_t first_update_id{0}; // `U`
    std::uint64_t last_update_id{0};  // `u`
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBookUpdate, WireError> parse_book_update(std::string_view frame,
                                                                         LevelBuffer& storage);

// spot.order_book: a whole book, not a delta. Reported through the same type
// with both ids set to `lastUpdateId`, because nothing downstream chains it —
// the point of the snapshot channel is that there is no chain to break.
[[nodiscard]] std::expected<WireBookUpdate, WireError> parse_book_snapshot(std::string_view frame,
                                                                           LevelBuffer& storage);

// From GET /futures/{settle}/order_book, and only usable as a seed when that
// request carried `with_id=true`: without it the body has no `id`, and levels
// with no id are a picture of no particular point in the update chain.
// `update_s` is FRACTIONAL seconds (1786394534.46), truncated on the way in.
struct WireOrderBook {
    std::uint64_t update_id{0}; // `id`
    std::uint64_t update_s{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireOrderBook, WireError> parse_order_book(std::string_view body,
                                                                       LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // book advanced and is publishable
    Buffered, // held until the seed lands
    Stale,    // wholly before the seed, or already applied
    Waiting,  // seeded, waiting for the frame that carries id + 1
    Gap,      // the contiguous chain broke: book dropped, re-seed required
    Reseed,   // the seed can no longer be reached: fetch another
};

// Deltas held while the REST seed is in flight. At a 100 ms cadence one round
// trip is a handful of frames; an overflow restarts the buffer rather than
// growing without bound.
inline constexpr std::size_t k_max_buffered_deltas = 64;

// One contract's book. Prices and sizes become instrument-scaled integers at
// ingest, so nothing downstream ever sees a venue string.
class DepthBook {
public:
    using Level = book::Level;

    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    // True while the book cannot publish and no snapshot is outstanding.
    [[nodiscard]] bool wants_seed() const noexcept;

    void seed_requested() noexcept { seed_in_flight_ = true; }

    void seed_failed() noexcept { seed_in_flight_ = false; }

    BookResult seed(const WireOrderBook& snapshot);
    BookResult apply(const WireBookUpdate& delta);

    [[nodiscard]] bool live() const noexcept { return state_ == State::Live; }

    // The book as shared memory takes it, or nullopt while it is unseeded or
    // desynchronised.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::uint64_t reseeds() const noexcept { return reseeds_; }

    [[nodiscard]] std::uint64_t dropped_levels() const noexcept { return dropped_levels_; }

    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_id_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    enum class State : std::uint8_t {
        Cold,     // no usable seed; deltas are buffered
        Bridging, // seed in hand, waiting for the frame that continues it
        Live,
    };

    struct Buffered {
        std::uint64_t first_id{0};
        std::uint64_t last_id{0};
        std::vector<Level> bids;
        std::vector<Level> asks;
    };

    [[nodiscard]] std::optional<Level> convert(const WireLevel& wire) const;
    static void upsert(book::Ladder& side, const Level& level);

    void apply_wire(std::span<const WireLevel> levels, book::Ladder& side);
    static void apply_levels(std::span<const Level> levels, book::Ladder& side);

    BookResult buffer(const WireBookUpdate& delta);
    BookResult bridge(const WireBookUpdate& delta);
    BookResult advance(const WireBookUpdate& delta);
    BookResult replay();
    BookResult settle();

    void install_held();
    void discard_held();

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::vector<Buffered> pending_;
    book::Ladders held_;
    std::uint64_t held_id_{0};
    std::uint64_t last_id_{0};
    std::uint64_t gaps_{0};
    std::uint64_t reseeds_{0};
    std::uint64_t dropped_levels_{0};
    State state_{State::Cold};
    bool seed_in_flight_{false};
};

} // namespace raikiri::venues::gate
