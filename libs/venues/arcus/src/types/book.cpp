#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/arcus/book.hpp>

namespace raikiri::venues::arcus {

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    streaming_ = false;
    crossed_ = false;
}

void Book::drop() noexcept {
    invalidate();
    resync_wanted_ = true;
}

bool Book::take_resync(std::uint64_t now_ns) noexcept {
    if (!resync_wanted_ || now_ns < next_resync_ns_) {
        return false;
    }
    resync_wanted_ = false;
    next_resync_ns_ = now_ns + k_resync_cooldown_ns;
    return true;
}

bool Book::apply_side(std::span<const WireLevel> levels, bool bid) {
    auto& side = bid ? ladders_.bids() : ladders_.asks();
    for (const auto& wire : levels) {
        const auto price_e9 = parse_decimal_e9(wire.price);
        const auto qty_e9 = parse_decimal_e9(wire.qty);
        if (!price_e9.has_value() || !qty_e9.has_value()) {
            return false;
        }
        const auto price = book::exact_steps(*price_e9, tick_size_);
        const auto qty = book::exact_steps(*qty_e9, lot_size_);
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

BookResult Book::settle() {
    const auto touch = ladders_.touch();
    crossed_ = touch.has_value() && touch->first.price.raw >= touch->second.price.raw;
    return crossed_ ? BookResult::Crossed : BookResult::Applied;
}

BookResult Book::reset(const WireBook& snapshot) {
    invalidate();
    if (!apply_side(snapshot.bids, true) || !apply_side(snapshot.asks, false)) {
        drop();
        return BookResult::Lost;
    }
    seq_ = snapshot.seq;
    synced_ = true;
    resync_wanted_ = false;
    const auto result = settle();
    return result == BookResult::Applied ? BookResult::Resynced : result;
}

BookResult Book::apply(const WireBook& delta) {
    if (!synced_ || delta.seq <= seq_) {
        return BookResult::Ignored;
    }
    if (streaming_ && delta.seq != seq_ + 1) {
        ++gaps_;
        drop();
        return BookResult::Lost;
    }
    if (!apply_side(delta.bids, true) || !apply_side(delta.asks, false)) {
        drop();
        return BookResult::Lost;
    }
    seq_ = delta.seq;
    streaming_ = true;
    return settle();
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || crossed_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, seq_, ts_event, ts_init);
}

} // namespace raikiri::venues::arcus
