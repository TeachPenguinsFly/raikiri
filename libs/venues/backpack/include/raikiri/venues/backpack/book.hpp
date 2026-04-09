#pragma once

// The order book: what a frame says about it, and the chaining rule a reader
// must hold to keep one in step.
//
// depth.<cadence>.<symbol> carries deltas and no snapshot, so the book is
// seeded from GET /api/v1/depth and then chained. The rules, measured against
// the live stream on 2026-09-24 rather than read off the docs, which state
// none of them:
//
//   * a SUCCESSOR chain: each frame's `U` is the previous frame's `u` + 1.
//     Held on every one of 5 167 consecutive realtime frames on BTC_USDC_PERP
//     and on every throttled frame across five symbols in the same run.
//   * the counter is per market and shared with bookTicker's `u`, and the REST
//     snapshot's `lastUpdateId` is a value on it — as a digit STRING, where
//     the stream writes a number.
//   * the snapshot's bids arrive worst first and its asks best first; the
//     ladder sorts, so nothing here depends on either.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/backpack/envelope.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::backpack {

// bookTicker.<symbol>: the touch alone, pushed on every change.
struct WireBookTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t update_id{0};
    std::uint64_t engine_us{0}; // T, MICROseconds
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view data);

// A RESTING size, read as what can be taken of it on the instrument's step:
// floored to whole lots, never refused for being finer.
//
// Refusing it would be the exact-or-nothing rule every price here follows,
// and it is the wrong rule for this number. Measured 2026-09-24: 23 of the 52
// Open spot books (and none of the 91 perpetual ones) held levels sized finer
// than the pair's own stepSize — 9.09 RAY on a step of 1, 1924.7 KMNO on a
// step of 10 — orders left resting from before the step was raised. Refused,
// those books never seed and those quotes never publish; floored, a level
// reads as the size an order on the current step can actually take, and one
// smaller than a step reads as zero. Prices are never read this way, and no
// price off its tick was seen anywhere.
[[nodiscard]] std::optional<std::int64_t> tradable_steps(std::string_view text,
                                                         FixedE9 step) noexcept;

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" deletes the level
};

inline constexpr std::size_t k_max_levels = 1000;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireDepthUpdate {
    std::string_view symbol;
    std::uint64_t first_id{0}; // U
    std::uint64_t last_id{0};  // u
    std::uint64_t engine_us{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view data,
                                                                           LevelBuffer& storage);

struct WireDepthSnapshot {
    std::uint64_t last_update_id{0};
    std::uint64_t timestamp_us{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthSnapshot, WireError>
parse_depth_snapshot(std::string_view body, LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // book advanced and is publishable
    Buffered, // held until the seed lands
    Stale,    // wholly before the seed, or already applied
    Gap,      // the chain broke: book dropped, re-seed required
    Reseed,   // the seed is older than the deltas it must join: fetch another
};

inline constexpr std::size_t k_max_buffered_deltas = 64;

// The seed depth, and so the width of the band the book is trusted inside.
// Matched to what a BookSnapshot holds. The venue takes only 5, 10, 20, 50,
// 100, 500 or 1000 and answers anything else with a 400, and with no limit at
// all it sends the whole book — 2 606 bids on BTC_USDC_PERP.
inline constexpr std::uint32_t k_seed_depth = 100;

class DepthBook {
public:
    using Level = book::Level;

    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] bool wants_seed() const noexcept;

    void seed_requested() noexcept { seed_in_flight_ = true; }

    void seed_failed() noexcept { seed_in_flight_ = false; }

    BookResult seed(const WireDepthSnapshot& snapshot);
    BookResult apply(const WireDepthUpdate& delta);

    [[nodiscard]] bool live() const noexcept { return state_ == State::Live; }

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::uint64_t reseeds() const noexcept { return reseeds_; }

    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_id_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    enum class State : std::uint8_t {
        Cold, // no usable seed; deltas are buffered
        Live,
    };

    struct Buffered {
        std::uint64_t first_id{0};
        std::uint64_t last_id{0};
        std::vector<Level> bids{};
        std::vector<Level> asks{};
    };

    [[nodiscard]] std::optional<Level> convert(const WireLevel& wire) const;
    static void upsert(book::Ladder& side, const Level& level);
    [[nodiscard]] bool apply_wire(std::span<const WireLevel> levels, book::Ladder& side);
    static void apply_levels(std::span<const Level> levels, book::Ladder& side);

    BookResult buffer(const WireDepthUpdate& delta);
    BookResult advance(const WireDepthUpdate& delta);
    BookResult step(const WireDepthUpdate& delta);
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
    PriceTicks band_low_{};
    PriceTicks band_high_{};
    std::uint64_t last_id_{0};
    std::uint64_t gaps_{0};
    std::uint64_t reseeds_{0};
    State state_{State::Cold};
    bool seed_in_flight_{false};
};

} // namespace raikiri::venues::backpack
