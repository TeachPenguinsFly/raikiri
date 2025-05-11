#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/gate/book.hpp>

#include <utility>

namespace raikiri::venues::gate {

namespace {

// The venue's own reader; the exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

bool DepthBook::wants_seed() const noexcept {
    return state_ == State::Cold && !seed_in_flight_;
}

void DepthBook::invalidate() noexcept {
    ladders_.clear();
    pending_.clear();
    held_.clear();
    held_id_ = 0;
    last_id_ = 0;
    state_ = State::Cold;
    seed_in_flight_ = false;
}

std::optional<DepthBook::Level> DepthBook::convert(const WireLevel& wire) const {
    const auto price = steps_of(wire.price, tick_size_);
    const auto qty = steps_of(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void DepthBook::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        // Size zero is Gate's delete, and it is also how a level leaving the
        // subscribed window is retired — half the frames carry nothing else.
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

void DepthBook::apply_wire(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto level = convert(wire);
        if (!level.has_value()) {
            ++dropped_levels_;
            continue;
        }
        upsert(side, *level);
    }
}

void DepthBook::apply_levels(std::span<const Level> levels, book::Ladder& side) {
    for (const auto& level : levels) {
        upsert(side, level);
    }
}

BookResult DepthBook::apply(const WireBookUpdate& delta) {
    switch (state_) {
    case State::Cold:
        return buffer(delta);
    case State::Bridging:
        return bridge(delta);
    case State::Live:
        return advance(delta);
    }
    return BookResult::Waiting;
}

BookResult DepthBook::buffer(const WireBookUpdate& delta) {
    if (!pending_.empty() && delta.first_update_id != pending_.back().last_id + 1) {
        pending_.clear(); // the chain broke before the seed landed
    }
    if (pending_.size() == k_max_buffered_deltas) {
        pending_.clear();
    }
    Buffered held{.first_id = delta.first_update_id, .last_id = delta.last_update_id};
    const auto take = [this](std::span<const WireLevel> src, std::vector<Level>& into) {
        into.reserve(src.size());
        for (const auto& wire : src) {
            const auto level = convert(wire);
            if (!level.has_value()) {
                ++dropped_levels_;
                continue;
            }
            into.push_back(*level);
        }
    };
    take(delta.bids, held.bids);
    take(delta.asks, held.asks);
    pending_.push_back(std::move(held));
    return BookResult::Buffered;
}

BookResult DepthBook::bridge(const WireBookUpdate& delta) {
    const auto wanted = held_id_ + 1;
    if (delta.last_update_id < wanted) {
        return BookResult::Stale; // every id here is already in the seed
    }
    if (delta.first_update_id > wanted) {
        // The stream has run past the seed and the chain is contiguous, so
        // nothing will ever fill what is now missing between them.
        discard_held();
        return BookResult::Reseed;
    }
    install_held();
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_update_id;
    return settle();
}

BookResult DepthBook::advance(const WireBookUpdate& delta) {
    if (delta.last_update_id <= last_id_) {
        return BookResult::Stale; // duplicate or reordered; already applied
    }
    if (delta.first_update_id != last_id_ + 1) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_update_id;
    return settle();
}

BookResult DepthBook::seed(const WireOrderBook& wire) {
    seed_in_flight_ = false;
    if (state_ == State::Live) {
        return BookResult::Stale; // a seed outlived the book it was meant for
    }
    held_.clear();
    apply_wire(wire.bids, held_.bids());
    apply_wire(wire.asks, held_.asks());
    if (held_.bids().empty() || held_.asks().empty()) {
        discard_held();
        return BookResult::Reseed;
    }
    held_id_ = wire.update_id;
    state_ = State::Bridging;
    return replay();
}

BookResult DepthBook::replay() {
    const auto wanted = held_id_ + 1;
    std::size_t i = 0;
    while (i < pending_.size() && pending_[i].last_id < wanted) {
        ++i;
    }
    if (i == pending_.size()) {
        pending_.clear();
        return BookResult::Waiting; // the seed is newer than anything buffered
    }
    if (pending_[i].first_id > wanted) {
        discard_held();
        return BookResult::Reseed;
    }

    install_held();
    // buffer() already enforced contiguity across pending_, so the replay only
    // has to find where the seed sits inside it.
    for (; i < pending_.size(); ++i) {
        apply_levels(pending_[i].bids, ladders_.bids());
        apply_levels(pending_[i].asks, ladders_.asks());
        last_id_ = pending_[i].last_id;
    }
    pending_.clear();
    return settle();
}

BookResult DepthBook::settle() {
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // Gate maintains the window, so a side emptying means what is held no
        // longer describes the venue's book; a fresh read is the way back.
        invalidate();
        return BookResult::Reseed;
    }
    return BookResult::Applied;
}

void DepthBook::install_held() {
    ladders_ = std::move(held_);
    held_.clear();
    last_id_ = held_id_;
    state_ = State::Live;
}

void DepthBook::discard_held() {
    held_.clear();
    held_id_ = 0;
    state_ = State::Cold;
    ++reseeds_;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_id_, ts_event, ts_init);
}

} // namespace raikiri::venues::gate
