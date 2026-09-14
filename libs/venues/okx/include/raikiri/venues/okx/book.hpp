#pragma once

// Order book maintenance for the `books` channel.
//
// OKX chains its frames on seqId: a delta belongs after the message whose
// seqId equals its prevSeqId, and a snapshot carries prevSeqId -1. That chain
// is the only continuity guarantee here — the checksum field was deprecated on
// 2026-06-23 and is pinned to 0, so it is not surfaced at all (see wire.hpp).
//
// The ladder underneath is raikiri::book's. What is not shared with another
// venue is the sequencing, which is OKX's, and what leaves the class is a
// snapshot the gateway can publish without the consumer knowing any of it.

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

// `books` pushes 400 levels a side on the snapshot; the ceiling leaves room
// without letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the seqId chain: a delta applies when its prev_seq_id equals
// the previous message's seq_id. The checksum field OKX still pushes was
// DEPRECATED on 2026-06-23 and is now pinned to 0 — validating it would
// resync forever against a constant, which is why it is not surfaced at all.
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

// One instrument's book. Venue strings become instrument-scaled integers at
// ingest, so nothing downstream ever sees a decimal from OKX.
class Book {
public:
    using Level = book::Level;

    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult apply(const WireBooks& update);

    // The top k_book_levels a side, or nullopt while the book is one-sided or
    // out of sync. A consumer cannot tell a stale book from a live one, so a
    // book we no longer trust is published as nothing at all.
    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::int64_t last_seq_id() const noexcept { return last_seq_id_; }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

    void invalidate() noexcept;

    // WHETHER TO ASK FOR THE SNAPSHOT AGAIN, and true at most once a backoff.
    // OKX sends a `books` snapshot only in answer to a subscribe, so a book
    // whose chain broke stays dark until something resubscribes it; nothing
    // did, and BTC-USDT-SWAP's cell on the box sat three hours old while its
    // quotes moved. Only a break this book detected counts: a socket reopen
    // re-asks for its own channels already.
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
