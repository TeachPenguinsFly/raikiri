#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/map.hpp>

namespace raikiri::venues::deribit {

namespace {

// The same exactness rule the mappers apply, through the same reader:
// Deribit writes sizes as "6.6e3" as readily as "6600.0", so the exponent
// form has to survive and book::exact_steps, which reads plain decimals,
// cannot. A remainder means Deribit moved a precision under us, and rounding
// it would quote a price the venue never published.
std::optional<std::int64_t> exact_steps(std::string_view token, FixedE9 step) {
    if (token.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(token);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

} // namespace

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    last_change_id_ = -1;
}

bool Book::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto price = exact_steps(wire.price, tick_size_);
        const auto qty = exact_steps(wire.qty, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        const book::Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};

        // The ACTION decides, not the size: `delete` is Deribit's removal and
        // is what the venue defines, where the zero beside it is only what it
        // currently sends. A `new` or `change` of zero is no level either.
        if (wire.action == LevelAction::Delete || level.qty.raw == 0) {
            side.remove(level.price);
            continue;
        }
        side.upsert(level);
    }
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        ladders_.clear();
        if (!apply_side(update.bids, ladders_.bids()) ||
            !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_change_id_ = update.change_id;
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // A delta before any snapshot has no book to chain onto.
        return BookResult::Ignored;
    }
    if (update.prev_change_id != last_change_id_) {
        if (update.change_id <= last_change_id_) {
            return BookResult::Ignored; // redelivery of a frame already applied
        }
        ++gaps_;
        invalidate();
        return BookResult::GapDetected;
    }

    if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    last_change_id_ = update.change_id;
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(
        instrument_, static_cast<std::uint64_t>(last_change_id_), ts_event, ts_init);
}

} // namespace raikiri::venues::deribit
