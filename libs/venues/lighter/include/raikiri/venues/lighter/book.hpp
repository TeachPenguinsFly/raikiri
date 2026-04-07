#pragma once

// The order book on order_book/<market_id>, which is also this venue's quote.
//
// VERIFIED against the live stream on 2026-09-24 rather than taken from the
// docs, though the docs say the same:
//
//   * the first frame is `subscribed/order_book` and carries the WHOLE book —
//     1682 bids and 1068 asks on BTC — so there is no REST seed and no depth
//     band to police; the book is the venue's, level for level.
//   * every later frame is `update/order_book`, a batch of changed levels
//     every 50 ms, where a size of zero deletes the level.
//   * frames chain on the matching engine's nonce: an update applies only when
//     its `begin_nonce` equals the previous frame's `nonce`. It held on 89 of
//     89 consecutive BTC frames. `offset` is the API server's counter, jumps on
//     a reconnect, and is not read.
//
// A broken chain cannot be repaired from what arrives next, because nothing
// but a subscription produces a snapshot; the book goes dark and the caller
// asks for it again.

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

// BTC's snapshot carried 1682 bids and 1068 asks on 2026-09-24, and every one
// of the 210 perpetual books seeded under this bound that day. A frame past it
// is refused rather than allowed to allocate, and the book stays dark.
inline constexpr std::size_t k_max_levels = 8192;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireOrderBook {
    std::uint32_t market_id{0};
    bool snapshot{false}; // subscribed/order_book, as against update/
    std::uint64_t nonce{0};
    std::uint64_t begin_nonce{0}; // 0 on the snapshot
    std::uint64_t updated_us{0};  // last_updated_at: MICROseconds
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireOrderBook, WireError> parse_order_book(std::string_view frame,
                                                                       LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Resynced, // a snapshot replaced the book
    Applied,  // an update chained onto it
    Ignored,  // an update with no book under it, or one already applied
    Gap,      // the nonce chain broke: book dropped, a snapshot is needed
    BadLevel, // a price or size off the instrument's grid: book dropped
};

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
