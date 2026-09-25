#pragma once

// seeded from GET /fapi/v1/depth, then chained on pu == the previous u;
// levels outside the seed's band are dropped

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::binance {

// pushed on every change, so it is the quote source
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

// on futures a delta chains when pu equals the previous event's u
struct WireDepthUpdate {
    std::string_view symbol;
    std::uint64_t first_id{0};
    std::uint64_t last_id{0};
    std::uint64_t prev_id{0};
    std::uint64_t event_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view data, LevelBuffer& storage);

// usable only while a delta still straddles last_update_id
struct WireDepthSnapshot {
    std::uint64_t last_update_id{0};
    std::uint64_t transaction_ms{0}; // T: when the book was that
    std::uint64_t event_ms{0};       // E: when binance answered
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthSnapshot, WireError> parse_depth_snapshot(std::string_view body,
                                                                               LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // book advanced and is publishable
    Buffered, // held until the seed lands
    Stale,    // wholly before the seed, or already applied
    Waiting,  // seeded, waiting for the delta that straddles the seed
    Gap,      // the pu chain broke: book dropped, re-seed required
    Reseed,   // the seed can no longer be bridged: fetch another
};

// an overflow restarts the buffer
inline constexpr std::size_t k_max_buffered_deltas = 64;

// matches k_book_levels; 100 levels cost weight 5 where 1000 cost 20
inline constexpr std::uint32_t k_seed_depth = 100;

// NoBook: the venue has no ladder for this symbol and a retry will not change that
enum class SeedAnswer : std::uint8_t { Book, Retry, RateLimited, NoBook };

[[nodiscard]] SeedAnswer classify_seed_answer(int status, std::string_view body) noexcept;

class DepthBook {
public:
    using Level = book::Level;

    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    // True while the book cannot publish and no snapshot is outstanding.
    [[nodiscard]] bool wants_seed() const noexcept;

    void seed_requested() noexcept { seed_in_flight_ = true; }

    void seed_failed() noexcept { seed_in_flight_ = false; }

    // no seed is asked again until readmit(), called by the catalogue refresh
    void seed_refused() noexcept {
        seed_in_flight_ = false;
        refused_ = true;
    }

    void readmit() noexcept { refused_ = false; }

    [[nodiscard]] bool refused() const noexcept { return refused_; }

    BookResult seed(const WireDepthSnapshot& snapshot);
    BookResult apply(const WireDepthUpdate& delta);

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
        Bridging, // seed in hand, waiting for the delta that straddles it
        Live,
    };

    struct Buffered {
        std::uint64_t first_id{0};
        std::uint64_t last_id{0};
        std::uint64_t prev_id{0};
        std::vector<Level> bids{};
        std::vector<Level> asks{};
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
    bool refused_{false};
};

} // namespace raikiri::venues::binance
