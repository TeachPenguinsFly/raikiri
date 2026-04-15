#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/lighter/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::lighter {

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0.00000" deletes the level
};

// A frame past this is refused, and the book stays dark.
inline constexpr std::size_t k_max_levels = 8192;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireOrderBook {
    std::uint32_t market_id{0};
    bool snapshot{false};
    std::uint64_t nonce{0};
    std::uint64_t begin_nonce{0}; // 0 on the snapshot
    std::uint64_t updated_us{0};  // microseconds
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireOrderBook, WireError> parse_order_book(std::string_view frame,
                                                                       LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Resynced,
    Applied,
    Ignored,  // an update with no book under it, or one already applied
    Gap,      // the nonce chain broke: book dropped, a snapshot is needed
    BadLevel, // a price or size off the instrument's grid: book dropped
};

// An update applies only when its begin_nonce equals the last nonce. `offset` is
// the API server's counter, jumps on a reconnect, and is not read.
class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireOrderBook& frame);

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    // Best bid and ask, or nullopt while unsynced or one-sided.
    [[nodiscard]] std::optional<Bbo> top(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    [[nodiscard]] std::uint64_t nonce() const noexcept { return nonce_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side) const;

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t nonce_{0};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::lighter
