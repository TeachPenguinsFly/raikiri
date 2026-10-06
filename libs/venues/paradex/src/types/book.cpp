#include <raikiri/venues/paradex/book.hpp>
#include <raikiri/venues/paradex/map.hpp>

namespace raikiri::venues::paradex {

Book::Book(const InstrumentBase& base) : base_{base} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    crossed_ = false;
}

BookResult Book::lose() noexcept {
    invalidate();
    resync_wanted_ = true;
    return BookResult::Lost;
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
        const auto level = map_level(wire, base_);
        if (!level.has_value()) {
            return false;
        }
        if (level->qty.raw == 0) {
            side.remove(level->price);
        } else {
            side.upsert(*level);
        }
    }
    return true;
}

BookResult Book::on_frame(const WireBook& wire) {
    if (wire.snapshot) {
        invalidate();
    } else if (!synced_ || wire.seq <= seq_) {
        return BookResult::Ignored;
    } else if (wire.seq != seq_ + 1) {
        ++gaps_;
        return lose();
    }
    if (!apply_side(wire.bids, true) || !apply_side(wire.asks, false)) {
        return lose();
    }
    seq_ = wire.seq;
    const bool fresh = !synced_;
    synced_ = true;
    resync_wanted_ = false;
    const auto touch = ladders_.touch();
    crossed_ = touch.has_value() && touch->first.price.raw >= touch->second.price.raw;
    if (crossed_) {
        return BookResult::Crossed;
    }
    return fresh ? BookResult::Resynced : BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || crossed_) {
        return std::nullopt;
    }
    return ladders_.snapshot(base_.id, static_cast<std::uint64_t>(seq_), ts_event, ts_init);
}

std::optional<Bbo> Book::top(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || crossed_) {
        return std::nullopt;
    }
    const auto touch = ladders_.touch();
    if (!touch.has_value()) {
        return std::nullopt;
    }
    return Bbo{.instrument = base_.id,
               .bid = touch->first.price,
               .ask = touch->second.price,
               .bid_qty = touch->first.qty,
               .ask_qty = touch->second.qty,
               .ts_event = ts_event,
               .ts_init = ts_init};
}

} // namespace raikiri::venues::paradex
