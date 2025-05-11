#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/binance/book.hpp>

#include <utility>

namespace raikiri::venues::binance {

namespace {

// The venue's own reader: binance writes plain decimals. The exact-or-refuse
// rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {
    ladders_.bids().reserve(k_seed_depth);
    ladders_.asks().reserve(k_seed_depth);
}

bool DepthBook::wants_seed() const noexcept {
    return state_ == State::Cold && !seed_in_flight_;
}

void DepthBook::invalidate() noexcept {
    ladders_.clear();
    pending_.clear();
    held_.clear();
    held_id_ = 0;
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
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
        // Size zero is binance's delete, on the seed and on the deltas alike.
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

BookResult DepthBook::apply(const WireDepthUpdate& delta) {
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

BookResult DepthBook::buffer(const WireDepthUpdate& delta) {
    if (!pending_.empty() && delta.prev_id != pending_.back().last_id) {
        pending_.clear(); // the chain broke before the seed landed
    }
    if (pending_.size() == k_max_buffered_deltas) {
        pending_.clear();
    }
    Buffered held{.first_id = delta.first_id, .last_id = delta.last_id, .prev_id = delta.prev_id};
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

BookResult DepthBook::bridge(const WireDepthUpdate& delta) {
    if (delta.last_id < held_id_) {
        return BookResult::Stale;
    }
    if (delta.first_id > held_id_) {
        // The stream has moved past the photograph, and no delta will ever
        // carry the one into the other.
        discard_held();
        return BookResult::Reseed;
    }
    install_held();
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_id;
    return settle();
}

BookResult DepthBook::advance(const WireDepthUpdate& delta) {
    if (delta.last_id <= last_id_) {
        return BookResult::Stale; // duplicate or reordered; already applied
    }
    if (delta.prev_id != last_id_) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_id;
    return settle();
}

BookResult DepthBook::seed(const WireDepthSnapshot& wire) {
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
    held_id_ = wire.last_update_id;
    band_low_ = held_.bids().levels().back().price;
    band_high_ = held_.asks().levels().back().price;
    state_ = State::Bridging;
    return replay();
}

BookResult DepthBook::replay() {
    std::size_t i = 0;
    while (i < pending_.size() && pending_[i].last_id < held_id_) {
        ++i;
    }
    if (i == pending_.size()) {
        pending_.clear();
        return BookResult::Waiting; // the seed is newer than anything buffered
    }
    if (pending_[i].first_id > held_id_) {
        discard_held();
        return BookResult::Reseed;
    }

    install_held();
    // buffer() already enforced the pu chain across pending_, so the replay
    // only has to find where the seed sits inside it.
    for (; i < pending_.size(); ++i) {
        apply_levels(pending_[i].bids, ladders_.bids());
        apply_levels(pending_[i].asks, ladders_.asks());
        last_id_ = pending_[i].last_id;
    }
    pending_.clear();
    return settle();
}

BookResult DepthBook::settle() {
    // Past the band the seed vouched for, a level is one no snapshot ever
    // confirmed; carrying it would make the book look deeper than it is known
    // to be.
    ladders_.bids().drop_beyond(band_low_);
    ladders_.asks().drop_beyond(band_high_);
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // The price walked clean out of the band, so what is left is no longer
        // the top of the venue's book.
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
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    state_ = State::Cold;
    ++reseeds_;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_id_, ts_event, ts_init);
}

} // namespace raikiri::venues::binance
