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
#include <raikiri/wire/error.hpp>

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

// UTA v2's `obu` channel, which carries a whole book and then deltas of it on
// ONE subscription and needs no REST seed at all.
//
// WHY A SECOND SHAPE EXISTS. Classic /contractMarket/level2 publishes one
// message per price level that moves, unthrottled: measured 2026-09-19 across
// KuCoin's 683 contracts, 15,380 messages a second against this channel's
// 3,230 for the same symbols — 4.76x — and the gateway cost 43.4% of a core
// against 8-18% for every other venue on the box. The reduction is 10 ms
// conflation, not lost data: counting distinct (symbol, 10 ms) pairs in the
// classic stream gives 3,452/s, which agrees within 4%.
//
// SEQUENCING IS A RANGE, NOT A NUMBER, and that is the part that does not
// port. Classic sends one `sequence` per single-level change and a reader
// requires `sequence == last + 1`. Here one frame carries many levels, so it
// names the span it covers and the venue's rule is:
//
//     start <= last_end + 1      (nothing was skipped)
//     end   >  last_end          (this is not a replay)
//
// A snapshot names `start == end`, which is the sequence already inside its
// levels — the same convention the REST seed used.
struct WireUtaBook {
    std::string_view symbol;
    bool is_snapshot{false};
    std::int64_t seq_start{0};
    std::int64_t seq_end{0};
    std::uint64_t ts_ns{0}; // matching-engine time, nanoseconds
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireUtaBook, WireError> parse_uta_book(std::string_view frame,
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

// One contract's book off UTA v2's `obu` channel.
//
// SEPARATE FROM DepthBook, and the separation is the point. DepthBook exists
// around a REST seed arriving in the middle of a stream: it buffers changes
// while cold, bridges them when the seed lands, and only then advances. Here
// the whole book arrives FIRST, on the same connection, ahead of every delta
// that refers to it — there is nothing to buffer, nothing to bridge, and no
// seed to fetch. Threading a second sequencing rule through that state machine
// would make both harder to read and leave no way back to the classic feed.
//
// THE CHAINING RULE IS THE VENUE'S, quoted rather than invented:
//
//     start <= last_end + 1      nothing was skipped
//     end   >  last_end          this is not a replay
//
// A snapshot names start == end, which is the sequence already inside its
// levels.
class UtaBook {
public:
    using Level = book::Level;

    UtaBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    BookResult apply(const WireUtaBook& frame);

    [[nodiscard]] bool live() const noexcept { return live_; }

    // Nothing at all while the chain is broken, under the same rule as every
    // other book here: a stale ladder is priced off as if it were current
    // while silence is visibly missing.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event,
                                                       UnixNanos ts_init) const;

    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::int64_t last_sequence() const noexcept { return last_end_; }

private:
    [[nodiscard]] bool load(std::span<const WireLevel> levels, book::Ladder& side);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::int64_t last_end_{0};
    std::uint64_t gaps_{0};
    bool live_{false};
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

    // HOW LONG A REFUSED SEED WAITS BEFORE ASKING AGAIN, and it exists because
    // without it the ask is bounded by the frame rate rather than by anything
    // sensible. /contractMarket/level2 pushes on every level that moves, and
    // the seed was asked for on every such frame for a book that had none: 677
    // books asked 27391 times in seven minutes on 2026-09-19 and were refused
    // 22861 of them with 429, which also starved the funding poll of the same
    // budget. A second is far shorter than a venue's rate window and far
    // longer than a frame.
    static constexpr std::uint64_t k_seed_retry_ns = 1'000'000'000ULL;

    // True while the book cannot publish, no snapshot is outstanding, and a
    // refused one has served its wait.
    [[nodiscard]] bool wants_seed(std::uint64_t now_ns) const noexcept;

    void seed_requested() noexcept { seed_in_flight_ = true; }

    void seed_failed(std::uint64_t now_ns) noexcept {
        seed_in_flight_ = false;
        seed_retry_at_ns_ = now_ns + k_seed_retry_ns;
    }

    BookResult seed(const WireOrderBook& snapshot);
    BookResult apply(const WireBookChange& change);

    [[nodiscard]] bool live() const noexcept { return state_ == State::Live; }

    // The book as shared memory takes it, or nullopt while it is unseeded or
    // desynchronised.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    // THE TOUCH, so the quote costs no second subscription. KuCoin's own
    // documentation says tickerV2 "shares the sn and sequence fields with the
    // Order Book — Incremental Updates topic, and the values of both fields
    // are identical": it is the same information arriving twice, and it was
    // 2325 messages a second of this gateway's load. bybit has been served
    // this way since it was written.
    //
    // Same rule as snapshot(): nothing at all while desynchronised, because a
    // stale quote is acted on as if it were current and silence is visibly
    // missing.
    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

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
    std::uint64_t seed_retry_at_ns_{0};
};

} // namespace raikiri::venues::kucoin
