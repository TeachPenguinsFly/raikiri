#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/blofin/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::blofin {

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0" deletes the level
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireBooks {
    std::string_view inst_id;
    bool snapshot{false};
    std::uint64_t seq_id{0};
    std::uint64_t prev_seq_id{0}; // "0" on a snapshot, where okx says -1
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBooks, WireError> parse_books(std::string_view frame,
                                                              LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Resynced,
    Applied,
    Ignored, // no book under it yet, or a delta the snapshot already holds
    Gap,     // the chain broke; only a fresh subscription brings a snapshot
    BadLevel,
};

class Book {
public:
    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBooks& frame);

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::optional<Bbo> top(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::span<const book::Level> bids() const noexcept {
        return ladders_.bids().levels();
    }

    [[nodiscard]] std::span<const book::Level> asks() const noexcept {
        return ladders_.asks().levels();
    }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side) const;

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t seq_id_{0};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::blofin
