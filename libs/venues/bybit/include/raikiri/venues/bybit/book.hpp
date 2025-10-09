#pragma once

// The order book: what a frame says about it, and what a reader must hold to
// keep one in step.
//
// THE TWO BELONG TOGETHER. The wire type means nothing without the chaining
// rule that decides whether a delta may be applied at all, and the rule means
// nothing without the field it reads.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

struct WireLevel {
    std::string_view price;
    std::string_view size; // "0" means delete this level
};

// orderbook.N.SYMBOL. `is_snapshot` distinguishes a full book from a delta;
// `update_id` and `seq` are bybit's ordering guarantees — a gap in update_id
// means the book must be resynchronised, which is why they are surfaced
// rather than swallowed.
struct WireOrderbook {
    std::string_view symbol;
    bool is_snapshot{false};
    std::uint64_t update_id{0};
    std::uint64_t seq{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

inline constexpr std::size_t k_max_levels = 512;

// Storage the caller owns and reuses across frames — parsing must not
// allocate on the market-data path.
struct OrderbookBuffer {
    WireLevel bids[k_max_levels];
    WireLevel asks[k_max_levels];
};

[[nodiscard]] std::expected<WireOrderbook, WireError> parse_orderbook(std::string_view frame,
                                                                      OrderbookBuffer& storage);

enum class SyncResult : std::uint8_t {
    Applied,     // book updated and coherent
    Resynced,    // snapshot replaced the book
    GapDetected, // update id skipped: book dropped, resubscribe required
    Ignored,     // stale message (update id already applied)
    BadLevel,    // a price or size failed exact fixed-point conversion
};

// One symbol's book. Prices and sizes are converted to instrument-scaled
// integers at ingest, so nothing downstream ever sees a venue string.
class BookSync {
public:
    using Level = book::Level;

    BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] SyncResult apply(const WireOrderbook& update);

    // Top of book, or nullopt while the book is empty or desynchronised.
    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

    // The top k_book_levels a side, under the same rule as bbo(): nothing at
    // all while desynchronised, because a stale book is priced off as if it
    // were current while silence is visibly missing data.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_update_id_; }

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
    std::uint64_t last_update_id_{0};
    std::uint64_t last_seq_{0};
    std::uint64_t gaps_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::bybit
