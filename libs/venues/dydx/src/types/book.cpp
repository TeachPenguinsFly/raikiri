#include <raikiri/venues/dydx/book.hpp>
#include <raikiri/venues/dydx/map.hpp>

namespace raikiri::venues::dydx {

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    bid_band_.reset();
    ask_band_.reset();
    synced_ = false;
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
    const auto& band = bid ? bid_band_ : ask_band_;
    for (const auto& wire : levels) {
        const auto price = exact_steps(wire.price, tick_size_);
        const auto qty = exact_steps(wire.size, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        const auto at = PriceTicks::from_raw(*price);
        if (band.has_value() && (bid ? at.raw < band->raw : at.raw > band->raw)) {
            ++outside_band_;
            continue;
        }
        if (*qty == 0) {
            side.remove(at);
            continue;
        }
        side.upsert({at, QtyLots::from_raw(*qty)});
    }
    return true;
}

BookResult Book::settle() {
    const auto touch = ladders_.touch();
    const bool crossed = touch.has_value() && touch->first.price.raw >= touch->second.price.raw;
    if (crossed && !crossed_) {
        ++crossings_;
    }
    crossed_ = crossed;
    resync_wanted_ = crossed;
    return crossed ? BookResult::Crossed : BookResult::Applied;
}

BookResult Book::reset(std::span<const WireLevel> bids, std::span<const WireLevel> asks) {
    invalidate();
    resync_wanted_ = false;
    if (!apply_side(bids, true) || !apply_side(asks, false)) {
        drop();
        return BookResult::Lost;
    }
    if (bids.size() >= k_snapshot_depth && !ladders_.bids().empty()) {
        bid_band_ = ladders_.bids().levels().back().price;
    }
    if (asks.size() >= k_snapshot_depth && !ladders_.asks().empty()) {
        ask_band_ = ladders_.asks().levels().back().price;
    }
    synced_ = true;
    return settle() == BookResult::Crossed ? BookResult::Crossed : BookResult::Resynced;
}

BookResult Book::apply(std::span<const WireLevel> bids, std::span<const WireLevel> asks) {
    if (!synced_) {
        return BookResult::Ignored;
    }
    if (!apply_side(bids, true) || !apply_side(asks, false)) {
        drop();
        return BookResult::Lost;
    }
    // A banded side that empties has walked off what the snapshot vouched for.
    if ((bid_band_.has_value() && ladders_.bids().empty()) ||
        (ask_band_.has_value() && ladders_.asks().empty())) {
        drop();
        return BookResult::Lost;
    }
    return settle();
}

std::optional<Bbo> Book::top(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || crossed_) {
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
    if (!synced_ || crossed_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, 0, ts_event, ts_init);
}

} // namespace raikiri::venues::dydx
