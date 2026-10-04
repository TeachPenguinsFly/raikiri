#pragma once

// seq steps by exactly one, snapshot included; a delta is one level at the root with its own side

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // 0 means delete this level
};

inline constexpr std::size_t k_max_levels = 4096;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// book_snapshot carries bids/asks arrays; a book delta is one level at the root
struct WireBook {
    std::string_view product_id;
    bool snapshot{false};
    std::uint64_t seq{0};
    std::uint64_t timestamp_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,     // delta chained onto the book we hold
    Resynced,    // book_snapshot replaced the book
    GapDetected, // seq skipped: book dropped until the next snapshot
    Ignored,     // a frame already applied, or a delta with no book yet
    BadLevel,    // a price or size failed exact fixed-point conversion
};

class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBook& update);

    // nullopt while the book is one-sided or out of sync
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t last_seq_{0};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

// spot v2: no sequence, so a crc32 of the top ten levels checks every message
struct WireSpotBook {
    std::string_view symbol;
    bool snapshot{false};
    std::uint32_t checksum{0};
    std::uint64_t ts_ns{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireSpotBook, WireError> parse_spot_book(std::string_view frame, LevelBuffer& storage);

// ieee crc32 of asks best-first then bids best-first, each level its price ticks then its lots in decimal
[[nodiscard]] std::uint32_t spot_book_checksum(std::span<const book::Level> bids, std::span<const book::Level> asks);

enum class SpotBookResult : std::uint8_t {
    Applied,
    Resynced,
    ChecksumMismatch, // the book is dropped and needs a fresh snapshot
    Ignored,          // an update with no snapshot yet
    BadLevel,
};

class SpotBook {
public:
    using Level = book::Level;

    SpotBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size, std::size_t depth);

    [[nodiscard]] SpotBookResult apply(const WireSpotBook& update);

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t mismatches() const noexcept { return mismatches_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    std::size_t depth_{0};
    book::Ladders ladders_;
    std::uint64_t updates_{0};
    std::uint64_t mismatches_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::kraken
