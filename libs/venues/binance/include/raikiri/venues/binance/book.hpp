#pragma once

// The order book: what a frame says about it, and what a reader must hold to
// keep one in step.
//
// THE TWO BELONG TOGETHER. The wire type means nothing without the chaining
// rule that decides whether a delta may be applied at all, and the rule means
// nothing without the field it reads.
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

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::binance {

// <sym>@bookTicker: the top of book alone, pushed on every change rather than
// on a cadence, which is why it is the quote source and @depth is the ladder.
struct WireBookTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t update_id{0};
    std::uint64_t transaction_ms{0}; // T: when the book changed
    std::uint64_t event_ms{0};       // E: when binance sent it
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view data);

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

inline constexpr std::size_t k_max_levels = 1000;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// first_id/last_id are binance's [U, u]; prev_id is `pu`. The chaining rule
// on futures is prev_id == the previous event's last_id — NOT last_id + 1,
// which is the spot rule and would reject every valid futures delta.
struct WireDepthUpdate {
    std::string_view symbol;
    std::uint64_t first_id{0};
    std::uint64_t last_id{0};
    std::uint64_t prev_id{0};
    std::uint64_t event_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view data,
                                                                           LevelBuffer& storage);

// GET /fapi/v1/depth. `last_update_id` is the book state these levels
// describe, and the seed is usable only while a delta still straddles it.
struct WireDepthSnapshot {
    std::uint64_t last_update_id{0};
    std::uint64_t transaction_ms{0}; // T: when the book was that
    std::uint64_t event_ms{0};       // E: when binance answered
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthSnapshot, WireError>
parse_depth_snapshot(std::string_view body, LevelBuffer& storage);

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
    using Level = book::Level;

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

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

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
    static void upsert(book::Ladder& side, const Level& level);

    void apply_wire(std::span<const WireLevel> levels, book::Ladder& side);
    static void apply_levels(std::span<const Level> levels, book::Ladder& side);

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
    book::Ladders ladders_;
    std::vector<Buffered> pending_;
    book::Ladders held_;
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
