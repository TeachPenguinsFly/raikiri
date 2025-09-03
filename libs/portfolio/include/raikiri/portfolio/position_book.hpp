#pragma once

// Every position the process holds, addressed by (strategy, instrument).
//
// A flat vector scanned linearly. Positions number in the tens, a scan over
// fifty 80-bit keys costs ~50ns, and fills — the only thing that looks a
// position up — arrive a few times a second, not a few times a microsecond.
// A hash table here would buy nanoseconds nobody can spend and cost a
// dependency the build does not have.
//
// Capacity is fixed at construction and the book refuses to grow. That is not
// about allocation: `find` hands out pointers that strategies hold across
// callbacks, and a reallocating vector would dangle them.
//
// There is deliberately no Portfolio type summing across positions. Realized
// PnL is denominated in each instrument's settlement currency — USDT on
// binance, BTC on deribit — and the currency is not modelled here, so an
// in-memory total would be adding unlike things. Totals belong in SQL, where
// the instrument row carries the symbols.

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/portfolio/position.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace raikiri::portfolio {

enum class BookError : std::uint8_t {
    Full, // capacity exhausted; size the book right
};

class PositionBook {
public:
    static constexpr std::size_t k_default_capacity = 256;

    explicit PositionBook(std::size_t capacity = k_default_capacity) : capacity_{capacity} {
        positions_.reserve(capacity);
    }

    [[nodiscard]] Position* find(PositionKey key) noexcept;
    [[nodiscard]] const Position* find(PositionKey key) const noexcept;

    [[nodiscard]] std::expected<Position*, BookError> open(PositionKey key, UnixNanos ts);

    // Puts back a position this process previously wrote to its journal. The
    // one way to seat a position without a fill, and it exists so a restart
    // comes up holding what it held rather than flat — a flat restart is a
    // reconciliation break against every instrument, which is the right alarm
    // for a real disagreement and the wrong one for an ordinary restart.
    //
    // Refuses a key already present: replay happens before trading, so a
    // collision means the caller replayed twice.
    [[nodiscard]] bool restore(PositionKey key, const Position::Restored& state);

    // Folds one fill into the position the event belongs to, opening it if this
    // is the first. `delta` is the caller's derived fill quantity — normally the
    // drop in leaves_qty, so a resent notification produces zero and changes
    // nothing. The strategy_id comes out of the event's own ClientOrderId, so
    // attribution needs no plumbing.
    [[nodiscard]] std::expected<Position::Applied, BookError>
    on_fill(const InstrumentBase& base, const OrderEvent& event, QtyLots delta);

    // Insertion order, and stable: the ledger and the tests both lean on it.
    [[nodiscard]] std::span<const Position> all() const noexcept { return positions_; }

    [[nodiscard]] std::size_t size() const noexcept { return positions_.size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::vector<Position> positions_;
    std::size_t capacity_;
};

} // namespace raikiri::portfolio
