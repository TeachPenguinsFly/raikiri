#pragma once

// seeded from the rest order_book, then chained on U == previous u + 1;
// the seed's id is the last update already applied, and gate trims the window itself

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

// sizes are integer contract counts; ts_ms is milliseconds
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

// the touch of every contract from the tickers poll; an empty side leaves its fields empty
struct TickerQuoteBuffer {
    std::array<WireBookTicker, 2048> rows{};
};

[[nodiscard]] std::expected<std::span<const WireBookTicker>, WireError>
parse_futures_tickers(std::string_view body, TickerQuoteBuffer& storage);

// a level is {p, s} on both the socket and the rest snapshot
struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// a delta applies when first_update_id is one past the previous last_update_id
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

// spot.order_book is a whole book; both ids are lastUpdateId
[[nodiscard]] std::expected<WireBookUpdate, WireError> parse_book_snapshot(std::string_view frame,
                                                                           LevelBuffer& storage);

// usable as a seed only with with_id=true; update_s is fractional seconds, truncated
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

// an overflow restarts the buffer
inline constexpr std::size_t k_max_buffered_deltas = 64;

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

    // nullopt while unseeded or desynchronised
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
        std::vector<Level> bids{};
        std::vector<Level> asks{};
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
