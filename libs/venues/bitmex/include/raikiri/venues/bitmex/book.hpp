#pragma once

// Order book maintenance for the `orderBookL2_25` table.
//
// BitMEX is the odd one of the delta venues: it has NO continuity token at
// all. A delta frame carries `table`, `action` and `data` and nothing else —
// no sequence, no previous sequence, no first/last pair — so there is no
// arithmetic that can tell a dropped frame from a quiet market. What replaces
// it is an id-keyed row set: the `partial` declares its own key as
// ["symbol","id","side"], and every later row names a level by that id rather
// than by price. The one divergence this DOES make visible is an `update`
// naming an id we do not hold, which can only mean a frame went missing, and
// that is what drops the book here.
//
// Two more facts, both measured on XBTUSD 2026-08-17 across 417 frames:
//
//   * BitMEX maintains the 25-level window itself, pairing each `insert` at
//     the touch with a `delete` of whatever fell off the far end. Nothing
//     here prunes, because there is nothing to prune — and 25 a side is the
//     deepest tier this venue offers below the 100 shm can hold.
//   * `update` carries only the columns that changed, so an absent `size` is
//     "unchanged", never zero. A `delete` carries no size at all.
//
// The sorted sides underneath are raikiri::book's ladder, but the id keying is
// not something a price ladder can hold, so the id-to-price index below sits
// beside it. What leaves the class is a snapshot the gateway can publish
// without the consumer knowing any of it.

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bitmex/envelope.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::bitmex {

// `id` is the venue's level handle and the key a `delete` names — BitMEX
// declares the row key as ["symbol","id","side"] in the partial's own `keys`
// field. A delete carries no size, hence qty optional like everything else.
// MEASURED 2026-08-17 on XBTUSD: a delete DOES also carry `price` today, but
// the id is what the venue defines and so what the book keys on.
//
// There is no sequence anywhere on this table: a delta frame carries only
// `table`, `action` and `data`, and the timestamp is per ROW.
struct WireLevel {
    std::string_view symbol;
    std::int64_t id{0};
    bool bid{false};
    std::string_view price;
    std::string_view qty;
    std::string_view ts;
};

// The snapshot is 25 a side; the ceiling leaves room without letting a hostile
// frame make us allocate.
inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> levels{};
};

[[nodiscard]] std::expected<std::span<const WireLevel>, WireError>
parse_book(std::string_view frame, LevelBuffer& storage);

enum class BookResult : std::uint8_t {
    Applied,  // rows applied onto the book we hold
    Resynced, // a `partial` replaced the book
    Diverged, // an update named an id we do not hold: book dropped
    Ignored,  // rows arriving before any partial
    BadLevel, // a price or size failed exact fixed-point conversion
};

// One symbol's book. Venue tokens become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from BitMEX.
class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(FrameAction action, std::span<const WireLevel> rows);

    // The top k_book_levels a side, or nullopt while the book is one-sided or
    // out of sync. A consumer cannot tell a stale book from a live one, so a
    // book we no longer trust is published as nothing at all.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::uint64_t divergences() const noexcept { return divergences_; }

    // Deletes naming an id the book never held. Harmless to skip — removing
    // nothing removes nothing — but a rising count means frames are being lost.
    [[nodiscard]] std::uint64_t stale_deletes() const noexcept { return stale_deletes_; }

    void invalidate() noexcept;

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    // An `update` or a `delete` names a row id and no price at all, which is
    // the one thing a price ladder cannot answer. This is the index that turns
    // an id into the price to key the ladder with; the sizes live there.
    struct Row {
        std::int64_t id{0};
        PriceTicks price{};
    };

    [[nodiscard]] book::Ladder& side_of(const WireLevel& row) noexcept;
    [[nodiscard]] std::vector<Row>& rows_of(const WireLevel& row) noexcept;

    [[nodiscard]] bool upsert(const WireLevel& row);
    [[nodiscard]] BookResult amend(const WireLevel& row);
    void erase(const WireLevel& row);

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::vector<Row> bid_rows_;
    std::vector<Row> ask_rows_;
    std::uint64_t divergences_{0};
    std::uint64_t stale_deletes_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::bitmex
