#include <raikiri/venues/edgex/book.hpp>

namespace raikiri::venues::edgex {

namespace {

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    version_ = 0;
}

bool Book::apply_side(std::span<const WireLevel> levels, book::Ladder& side) const {
    for (const auto& wire : levels) {
        const auto price = steps_of(wire.price, tick_size_);
        const auto qty = steps_of(wire.size, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        if (*qty == 0) {
            side.remove(PriceTicks::from_raw(*price));
            continue;
        }
        side.upsert({PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)});
    }
    return true;
}

BookResult Book::apply(const WireDepth& frame) {
    if (frame.snapshot) {
        ladders_.clear();
        if (!apply_side(frame.bids, ladders_.bids()) || !apply_side(frame.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        version_ = frame.end_version;
        synced_ = true;
        return BookResult::Resynced;
    }
    if (!synced_) {
        return BookResult::Ignored;
    }
    if (frame.start_version != version_) {
        if (frame.end_version <= version_) {
            return BookResult::Ignored;
        }
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    if (!apply_side(frame.bids, ladders_.bids()) || !apply_side(frame.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    version_ = frame.end_version;
    // The 15-level window is the server's to keep; a crossed top means it did
    // not, and no delta that follows can say which side is wrong.
    if (const auto touch = ladders_.touch();
        touch.has_value() && touch->first.price >= touch->second.price) {
        ++gaps_;
        invalidate();
        return BookResult::Crossed;
    }
    return BookResult::Applied;
}

std::optional<Bbo> Book::top(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    const auto touch = ladders_.touch();
    if (!touch.has_value()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = instrument_;
    out.bid = touch->first.price;
    out.bid_qty = touch->first.qty;
    out.ask = touch->second.price;
    out.ask_qty = touch->second.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, version_, ts_event, ts_init);
}

} // namespace raikiri::venues::edgex
