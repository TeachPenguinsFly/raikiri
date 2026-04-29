#include <raikiri/venues/extended/book.hpp>

namespace raikiri::venues::extended {

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
    seq_ = 0;
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

BookResult Book::apply(const WireBook& frame) {
    if (frame.snapshot) {
        ladders_.clear();
        synced_ = true;
    } else if (!synced_) {
        return BookResult::Ignored;
    }
    if (!apply_side(frame.bids, ladders_.bids()) || !apply_side(frame.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    seq_ = frame.seq;
    return frame.snapshot ? BookResult::Resynced : BookResult::Applied;
}

std::optional<Bbo> Book::top(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    const auto touch = ladders_.touch();
    if (!touch.has_value()) {
        return std::nullopt;
    }
    return Bbo{.instrument = instrument_,
               .bid = touch->first.price,
               .ask = touch->second.price,
               .bid_qty = touch->first.qty,
               .ask_qty = touch->second.qty,
               .ts_event = ts_event,
               .ts_init = ts_init};
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, seq_, ts_event, ts_init);
}

std::optional<Bbo> map_top(const WireBook& frame,
                           InstrumentId instrument,
                           FixedE9 tick_size,
                           FixedE9 lot_size,
                           UnixNanos ts_init) {
    if (frame.bids.empty() || frame.asks.empty()) {
        return std::nullopt;
    }
    const auto bid = steps_of(frame.bids.front().price, tick_size);
    const auto bid_qty = steps_of(frame.bids.front().size, lot_size);
    const auto ask = steps_of(frame.asks.front().price, tick_size);
    const auto ask_qty = steps_of(frame.asks.front().size, lot_size);
    if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() || !ask_qty.has_value() ||
        *bid_qty == 0 || *ask_qty == 0) {
        return std::nullopt;
    }
    return Bbo{.instrument = instrument,
               .bid = PriceTicks::from_raw(*bid),
               .ask = PriceTicks::from_raw(*ask),
               .bid_qty = QtyLots::from_raw(*bid_qty),
               .ask_qty = QtyLots::from_raw(*ask_qty),
               .ts_event = frame.ts_ms == 0 ? ts_init : UnixNanos{frame.ts_ms * 1'000'000ULL},
               .ts_init = ts_init};
}

} // namespace raikiri::venues::extended
