#pragma once

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

// a gap in update_id requires a resync
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

// caller-owned storage reused across frames so parsing does not allocate
struct OrderbookBuffer {
    WireLevel bids[k_max_levels];
    WireLevel asks[k_max_levels];
};

[[nodiscard]] std::expected<WireOrderbook, WireError> parse_orderbook(std::string_view frame, OrderbookBuffer& storage);

enum class SyncResult : std::uint8_t {
    Applied,     // book updated and coherent
    Resynced,    // snapshot replaced the book
    GapDetected, // update id skipped: book dropped, resubscribe required
    Ignored,     // stale message (update id already applied)
    BadLevel,    // a price or size failed exact fixed-point conversion
};

class BookSync {
public:
    using Level = book::Level;

    BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] SyncResult apply(const WireOrderbook& update);

    // top of book, or nullopt while empty or desynchronised
    [[nodiscard]] std::optional<Bbo> bbo(UnixNanos ts_event, UnixNanos ts_init) const;

    // nothing while desynchronised, same as bbo()
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
