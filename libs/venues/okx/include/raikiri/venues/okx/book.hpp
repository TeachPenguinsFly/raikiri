#pragma once

// frames chain on seqId and a snapshot has prevSeqId -1; the checksum is pinned to 0 and not read

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/okx/errors.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::venues::okx {

struct WireBbo {
    std::string_view inst_id;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::int64_t seq_id{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// snapshots carry 400 levels a side
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireBooks {
    std::string_view inst_id;
    bool snapshot{false}; // action: "snapshot" vs "update"
    std::int64_t seq_id{0};
    std::int64_t prev_seq_id{0}; // -1 on a snapshot
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBooks, WireError> parse_books(std::string_view frame,
                                                              LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,     // delta chained onto the book we hold
    Resynced,    // snapshot replaced the book
    GapDetected, // the seqId chain broke: book dropped until the next snapshot
    Ignored,     // a frame the book already contains, or one with no book yet
    BadLevel,    // a price or size failed exact fixed-point conversion
};

class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBooks& update);

    // nullopt while the book is one-sided or out of sync
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    void invalidate() noexcept;

    // okx sends a snapshot only on subscribe, so a broken chain needs a resubscribe;
    // true at most once per backoff
    [[nodiscard]] bool resync_due(std::uint64_t now_ns) noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::int64_t last_seq_id_{-1};
    std::uint64_t gaps_{0};
    std::uint64_t resync_asked_ns_{0};
    bool synced_{false};
    bool broken_{false};
};

inline constexpr std::uint64_t k_resync_backoff_ns = 10'000'000'000ULL;

} // namespace raikiri::venues::okx
