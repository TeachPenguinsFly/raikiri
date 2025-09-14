#pragma once

// The order book: what a frame says about it, and what a reader must hold to
// keep one in step.
//
// THE TWO BELONG TOGETHER. The wire type means nothing without the chaining
// rule that decides whether a change may be applied at all, and the rule means
// nothing without the field it reads.
//
// KuCoin sends no snapshot on /contractMarket/level2 at all, so
// GET /api/v1/level2/snapshot is not an optimisation, it is the only way to
// start. Four things measured off the live socket on XBTUSDTM, none of them
// taken from the docs:
//
//   * one frame is ONE level, packed as "64070.1,buy,154" — price, side, size.
//     There is no array anywhere on this topic, so a delta is a single upsert
//     and buffering during the seed round trip costs one entry per frame.
//   * the chain is exactly contiguous: `sequence == the previous frame's + 1`
//     held on 6803 of 6803 consecutive frames. The seed's `sequence` is the
//     last change already inside its levels, so the frame that carries them
//     forward is the one holding sequence + 1.
//   * the stream runs at ~340 frames a second, two orders above gate's, which
//     is why the buffer here holds a thousand and not gate's sixty-four.
//   * the seed is not a window the venue maintains. A snapshot fetched
//     alongside a fresh subscription came back 1555 sequences BEHIND the first
//     streamed frame, so a seed that cannot be reached is discarded rather
//     than spliced, and levels outside the band the seed vouched for are
//     dropped the way binance's are — nothing else can confirm them.
//
// The ladder underneath is raikiri::book's. What is not shared with another
// venue is the sequencing, which is KuCoin's, and what leaves the class is a
// snapshot the gateway can publish without the consumer knowing any of it.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::kucoin {

// Sizes are CONTRACTS and arrive as bare integers; they still travel as text so
// the mapper divides by the instrument's lot step rather than assuming the
// lotSize of 1 that KuCoin happens to list on every contract today.
struct WireBbo {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame,
                                                          MarketType market);

// One level2 frame is ONE level packed into a comma-joined string:
// "62234.6,buy,1" is price, side, size, and a size of 0 deletes the level. The
// stream carries no snapshot; a book must start from GET /api/v1/level2/snapshot
// and replay from `sequence`.
struct WireBookChange {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool bid{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBookChange, WireError> parse_book_change(std::string_view frame);

// GET /api/v1/level2/snapshot. The only place a KuCoin futures book can start.
// Levels are [price, size] pairs of JSON NUMBERS, so the comma-joined string of
// the stream has no counterpart here. `sequence` is the last change already
// inside these levels, so the frame that carries them forward is the one
// holding sequence + 1.
struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// 1000 a side measured on XBTUSDTM; the ceiling leaves room without letting a
// body make us allocate.
inline constexpr std::size_t k_max_levels = 2048;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// The spot depth channel republishes a whole book instead of packing one level
// change into a string. Both sides arrive best-first.
struct WireBookSnapshot {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBookSnapshot, WireError> parse_book_snapshot(std::string_view frame,
                                                                             LevelBuffer& storage);

// `ts` here is NANOSECONDS, where every other KuCoin timestamp is milliseconds.
struct WireOrderBook {
    std::string_view symbol;
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
    std::span<const WireLevel> bids; // descending
    std::span<const WireLevel> asks; // ascending
};

[[nodiscard]] std::expected<WireOrderBook, WireError> parse_level2_snapshot(std::string_view body,
                                                                            LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // book advanced and is publishable
    Buffered, // held until the seed lands
    Stale,    // already inside the seed, or already applied
    Waiting,  // seeded, waiting for the frame that carries sequence + 1
    Gap,      // the contiguous chain broke: book dropped, re-seed required
    Reseed,   // the seed can no longer be reached: fetch another
    BadLevel, // a price or size failed exact fixed-point conversion
};

// Changes held while the REST seed is in flight. One entry is one level, and
// at the measured rate a slow round trip is a few hundred; an overflow
// restarts the buffer rather than growing without bound.
inline constexpr std::size_t k_max_buffered_changes = 1024;

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
    BookResult apply(const WireBookChange& change);

    [[nodiscard]] bool live() const noexcept { return state_ == State::Live; }

    // The book as shared memory takes it, or nullopt while it is unseeded or
    // desynchronised.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::uint64_t reseeds() const noexcept { return reseeds_; }

    [[nodiscard]] std::int64_t last_sequence() const noexcept { return last_sequence_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    enum class State : std::uint8_t {
        Cold,     // no usable seed; changes are buffered
        Bridging, // seed in hand, waiting for the change that continues it
        Live,
    };

    struct Buffered {
        std::int64_t sequence{0};
        Level level{};
        bool bid{false};
    };

    [[nodiscard]] std::optional<Level> convert(std::string_view price, std::string_view qty) const;
    static void upsert(book::Ladder& side, const Level& level);

    void place(const Level& level, bool bid);

    BookResult buffer(std::int64_t sequence, const Level& level, bool bid);
    BookResult bridge(std::int64_t sequence, const Level& level, bool bid);
    BookResult advance(std::int64_t sequence, const Level& level, bool bid);
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
    std::int64_t held_sequence_{0};
    std::int64_t last_sequence_{0};
    PriceTicks band_low_{};  // the deepest bid the seed vouched for
    PriceTicks band_high_{}; // and the highest ask
    std::uint64_t gaps_{0};
    std::uint64_t reseeds_{0};
    State state_{State::Cold};
    bool seed_in_flight_{false};
};

} // namespace raikiri::venues::kucoin
