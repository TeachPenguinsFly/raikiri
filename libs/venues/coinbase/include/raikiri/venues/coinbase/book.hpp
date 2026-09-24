#pragma once

// level2_batch: the snapshot is the whole book, cut at k_max_levels into a trusted band;
// changes carry no sequence, so a dropped frame is invisible

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// ["price","size"] decimal strings; size zero deletes and appears only on updates
struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// sides arrive sorted best-first; reading stops here and the book bands at the last level kept
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireL2Snapshot {
    std::string_view product_id;
    std::string_view time; // ISO-8601; absent on the snapshot in some captures
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireL2Snapshot, WireError> parse_l2_snapshot(std::string_view frame,
                                                                         LevelBuffer& storage);

// ["buy"|"sell","price","size"]; the side is a word and the size absolute
struct WireChange {
    std::string_view price;
    std::string_view qty;
    bool bid{false};
};

// observed batches are a few dozen
inline constexpr std::size_t k_max_changes = 4096;

struct ChangeBuffer {
    std::array<WireChange, k_max_changes> changes{};
};

// no sequence on this channel
struct WireL2Update {
    std::string_view product_id;
    std::string_view time; // ISO-8601
    std::span<const WireChange> changes;
};

[[nodiscard]] std::expected<WireL2Update, WireError> parse_l2_update(std::string_view frame,
                                                                     ChangeBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // changes applied onto the book we hold
    Resynced, // the opening snapshot replaced the book
    Ignored,  // a change arriving before any snapshot
    BadLevel, // a price or size failed exact fixed-point conversion
};

class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    BookResult reset(const WireL2Snapshot& snapshot);
    BookResult apply(const WireL2Update& update);

    // nullopt while one-sided or out of sync
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    // changes at prices outside the snapshot band; expected, worth watching
    [[nodiscard]] std::uint64_t outside_band() const noexcept { return outside_band_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    [[nodiscard]] std::optional<Level> convert(const WireLevel& wire) const;
    static void upsert(book::Ladder& side, const Level& level);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    PriceTicks band_low_{};  // the deepest bid the snapshot vouched for
    PriceTicks band_high_{}; // and the highest ask
    std::uint64_t outside_band_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::coinbase
