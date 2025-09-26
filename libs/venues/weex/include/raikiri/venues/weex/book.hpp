#pragma once

// The order book: what a frame says about it, and what a reader must hold to
// keep one in step.
//
// THE TWO BELONG TOGETHER. The wire type means nothing without the chaining
// rule that decides whether a delta may be applied at all, and the rule means
// nothing without the ids it reads.
//
// <sym>@depth15 SEEDS ITSELF. The channel opens with an `e:"depthSnapshot"`
// carrying the whole window and continues as `e:"depth"` deltas, so there is no
// REST read in this path at all. Four things measured off the live socket on
// 2026-09-07, none of them taken from a page:
//
//   * the chain shares its boundary: `U == the previous frame's u`, on 478 of
//     478 consecutive deltas across four books in 75 s, with no id skipped and
//     none repeated. `U == previous u + 1` held on none of them and would
//     reject every valid frame.
//   * the window is maintained by the venue. Subscribed at 15, the book stood
//     at exactly 15 levels a side on 595 of 595 frames applied — WEEX deletes
//     what falls out of it by sending the level at quantity "0". Nothing here
//     prunes, because there is nothing to prune.
//   * every price and every size divided exactly by the catalogue's own
//     10^-pricePrecision and 10^-quantityPrecision on 6092 of 6092 levels, so
//     an exact-or-refuse conversion drops nothing that WEEX actually sends.
//   * a re-SUBSCRIBE of a channel a live socket already holds draws a fresh
//     `depthSnapshot` within 300 ms. That is the recovery a gap would need; the
//     connection pool has no per-key resend today, so a gapped book is
//     invalidated and waits for its socket's next open, which re-asks and is
//     answered with a snapshot the same way.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/weex/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

// A level is ["79375.3","2.5048"] on the socket and in the REST body alike.
struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// The deepest tier WEEX offers is 200 — `@depth5` and `@depth20` are refused
// with `invalid depth level`, so 15 and 200 are the whole list. This adapter
// subscribes at 15 and the buffer still holds 200, because a frame arriving
// from a tier nobody asked for must be readable before it can be judged.
inline constexpr std::size_t k_max_levels = 200;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// One frame off the depth channel, snapshot or delta.
//
// `d` is the venue's own word for which it is — "SNAPSHOT" or "CHANGED" — and
// `e` says the same thing a second time. The parser refuses a frame whose two
// answers disagree: a snapshot applied as a delta leaves a book that is a
// mixture of two moments and reports itself as live.
struct WireDepth {
    std::string_view symbol;
    bool is_snapshot{false};
    std::uint64_t first_id{0}; // U
    std::uint64_t last_id{0};  // u
    std::uint64_t event_ms{0}; // E
    std::uint32_t tier{0};     // l: the depth tier the venue is answering at
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // book advanced and is publishable
    Unseeded, // a delta before the channel's own snapshot: dropped, never guessed
    Stale,    // wholly before the current state, or already applied
    Gap,      // the shared-boundary chain broke: book dropped, snapshot required
};

// One contract's book. Prices and sizes become instrument-scaled integers at
// ingest, so nothing downstream ever sees a venue string.
class DepthBook {
public:
    using Level = book::Level;

    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    BookResult apply(const WireDepth& frame);

    [[nodiscard]] bool live() const noexcept { return live_; }

    // The book as shared memory takes it, or nullopt while it is unseeded or
    // desynchronised.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    // The top of book off the same ladders. WEEX streams no quote channel at
    // all — `@bookTicker` is refused with `invalid event : bookticker` — so
    // this IS the venue's BBO rather than a cheaper restatement of one.
    [[nodiscard]] std::optional<Bbo> touch(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::uint64_t dropped_levels() const noexcept { return dropped_levels_; }

    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_id_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] std::optional<Level> convert(const WireLevel& wire) const;
    static void upsert(book::Ladder& side, const Level& level);
    void apply_wire(std::span<const WireLevel> levels, book::Ladder& side);

    BookResult seed(const WireDepth& frame);
    BookResult advance(const WireDepth& frame);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t last_id_{0};
    std::uint64_t gaps_{0};
    std::uint64_t dropped_levels_{0};
    bool live_{false};
};

} // namespace raikiri::venues::weex
