#pragma once

// book frames carry no sequence or timestamp, so a dropped frame is undetectable
// a level is [price, count, amount]: amount's sign is the side; count 0 deletes

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// count 0 deletes; amount's sign is the side, and on a delete it is just +1/-1
inline constexpr std::size_t k_level_arity = 3;
inline constexpr std::size_t k_level_price = 0;
inline constexpr std::size_t k_level_count = 1;
inline constexpr std::size_t k_level_amount = 2;

struct WireLevel {
    std::string_view price;
    std::string_view amount;
    std::int64_t count{0};
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> levels{};
};

struct WireBook {
    bool snapshot{false};
    std::span<const WireLevel> levels;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // levels applied onto the book we hold
    Resynced, // the opening array-of-arrays replaced the book
    Ignored,  // a delta arriving before any snapshot
    BadLevel, // a price or size failed exact fixed-point conversion
};

class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBook& update);

    // nullopt while one-sided or out of sync
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    // the touch from the book; the ticker channel updates far less often
    [[nodiscard]] std::optional<Bbo> touch(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_level(const WireLevel& wire);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    bool synced_{false};
};

} // namespace raikiri::venues::bitfinex
