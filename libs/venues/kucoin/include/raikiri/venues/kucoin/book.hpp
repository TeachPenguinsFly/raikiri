#pragma once

// level2 sends no snapshot, so a book starts from the rest seed and chains on sequence == previous + 1

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

// sizes are contracts; divide by the lot step instead of assuming lot size 1
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

// one level per frame, "price,side,size"; size 0 deletes the level
struct WireBookChange {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool bid{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBookChange, WireError> parse_book_change(std::string_view frame);

// levels are json numbers; the first frame to apply after the seed is sequence + 1
struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 2048;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// both sides arrive best-first
struct WireBookSnapshot {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBookSnapshot, WireError> parse_book_snapshot(std::string_view frame,
                                                                             LevelBuffer& storage);

// chain rule: start <= last_end + 1 and end > last_end; a snapshot has start == end
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

// ts here is nanoseconds; other kucoin timestamps are milliseconds
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

// one contract's book off the uta v2 obu channel; the snapshot arrives first, so nothing is buffered
class UtaBook {
public:
    using Level = book::Level;

    UtaBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    BookResult apply(const WireUtaBook& frame);

    [[nodiscard]] bool live() const noexcept { return live_; }

    // nothing while the chain is broken
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

// changes held while the rest seed is in flight; an overflow restarts the buffer
inline constexpr std::size_t k_max_buffered_changes = 1024;

class DepthBook {
public:
    using Level = book::Level;

    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    // wait between refused seed requests, so asking is not paced by the frame rate
    static constexpr std::uint64_t k_seed_retry_ns = 1'000'000'000ULL;

    // true while unpublishable, no seed outstanding and any refusal wait served
    [[nodiscard]] bool wants_seed(std::uint64_t now_ns) const noexcept;

    void seed_requested() noexcept { seed_in_flight_ = true; }

    void seed_failed(std::uint64_t now_ns) noexcept {
        seed_in_flight_ = false;
        seed_retry_at_ns_ = now_ns + k_seed_retry_ns;
    }

    BookResult seed(const WireOrderBook& snapshot);
    BookResult apply(const WireBookChange& change);

    [[nodiscard]] bool live() const noexcept { return state_ == State::Live; }

    // nullopt while unseeded or desynchronised
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    // tickerV2 repeats the book's top, so the quote comes from here; nothing while desynchronised
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
