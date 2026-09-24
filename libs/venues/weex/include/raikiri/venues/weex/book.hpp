#pragma once

// @depth200 opens with a depthSnapshot then deltas chained on U == previous u
// only a resubscribe draws a new snapshot

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

// a level is ["price","qty"] on the socket and in rest
struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// weex offers depth 15 and 200 only; the top k_book_levels are published
inline constexpr std::size_t k_max_levels = 200;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// d and e both name snapshot or delta; a frame where they disagree is refused
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

class DepthBook {
public:
    using Level = book::Level;

    DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    BookResult apply(const WireDepth& frame);

    [[nodiscard]] bool live() const noexcept { return live_; }

    // nullopt while unseeded or desynchronised
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    // weex streams no quote channel, so this is the venue's bbo
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

// weex sends a snapshot only on subscribe and nothing while a book is still
// a silent reseed whose snapshot matches doubles the wait up to the cap
inline constexpr std::uint64_t k_book_silence_ns = 300ULL * 1'000'000'000ULL;
inline constexpr std::uint64_t k_book_silence_cap_ns = 3'600ULL * 1'000'000'000ULL;

enum class Reseed : std::uint8_t { None, Gap, Silent, StillSilent };

struct BookWatch {
    InstrumentId instrument{};
    std::uint64_t seen_ns{0};
    std::uint64_t asked_ns{0};
    std::uint64_t silence_ns{k_book_silence_ns};
    std::uint64_t digest{0}; // the book a silent reseed replaced; 0 when none is pending
    bool gapped{false};
    bool quiet{false};
};

[[nodiscard]] Reseed due_for_reseed(BookWatch& watch, std::uint64_t now_ns) noexcept;
void on_delta(BookWatch& watch, std::uint64_t now_ns) noexcept;
// true when the reseed snapshot differs from the book it replaced
[[nodiscard]] bool on_reseed_snapshot(BookWatch& watch, std::uint64_t digest) noexcept;
[[nodiscard]] std::uint64_t book_digest(const DepthBook& book) noexcept;

} // namespace raikiri::venues::weex
