#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/cryptocom/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::cryptocom {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" deletes the level
};

inline constexpr std::size_t k_max_levels = 64;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireBook {
    bool snapshot{false};
    std::uint64_t seq{0};      // u
    std::uint64_t prev_seq{0}; // pu, deltas only
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

// the envelope's channel says which shape the frame is
[[nodiscard]] std::expected<WireBook, WireError>
parse_book(std::string_view data, bool snapshot, LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,
    Resynced,
    Gap, // pu broke the chain: unsynced until a new snapshot
    Ignored,
    BadLevel, // a level off the instrument's tick or lot
};

// the venue keeps the 50-level window and deletes what falls out
class Book {
public:
    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBook& update);

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    void invalidate() noexcept;

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

} // namespace raikiri::venues::cryptocom
