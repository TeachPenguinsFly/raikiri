#include <raikiri/venues/gate/book.hpp>

#include <algorithm>
#include <utility>

namespace raikiri::venues::gate {

namespace {

// Venue tokens convert through the decimal parser and then divide exactly by
// the instrument's step. A remainder means Gate changed a contract's precision,
// so the level is dropped rather than rounded — rounding is how a book ends up
// quoting a price the venue never showed.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

} // namespace

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

bool DepthBook::wants_seed() const noexcept {
    return state_ == State::Cold && !seed_in_flight_;
}

void DepthBook::invalidate() noexcept {
    bids_.clear();
    asks_.clear();
    pending_.clear();
    held_bids_.clear();
    held_asks_.clear();
    held_id_ = 0;
    last_id_ = 0;
    state_ = State::Cold;
    seed_in_flight_ = false;
}

std::optional<DepthBook::Level> DepthBook::convert(const WireLevel& wire) const {
    const auto price = exact_steps(wire.price, tick_size_);
    const auto qty = exact_steps(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void DepthBook::upsert(std::vector<Level>& side, bool descending, const Level& level) {
    const auto cmp = [descending](const Level& a, const Level& b) {
        return descending ? a.price.raw > b.price.raw : a.price.raw < b.price.raw;
    };
    const auto pos = std::lower_bound(side.begin(), side.end(), level, cmp);
    const bool exists = pos != side.end() && pos->price.raw == level.price.raw;

    if (level.qty.raw == 0) {
        // Size zero is Gate's delete, and it is also how a level leaving the
        // subscribed window is retired — half the frames carry nothing else.
        if (exists) {
            side.erase(pos);
        }
        return;
    }
    if (exists) {
        pos->qty = level.qty;
    } else {
        side.insert(pos, level);
    }
}

void DepthBook::apply_wire(std::span<const WireLevel> levels,
                           std::vector<Level>& side,
                           bool descending) {
    for (const auto& wire : levels) {
        const auto level = convert(wire);
        if (!level.has_value()) {
            ++dropped_levels_;
            continue;
        }
        upsert(side, descending, *level);
    }
}

void DepthBook::apply_levels(std::span<const Level> levels,
                             std::vector<Level>& side,
                             bool descending) {
    for (const auto& level : levels) {
        upsert(side, descending, level);
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
    apply_wire(delta.bids, bids_, /*descending=*/true);
    apply_wire(delta.asks, asks_, /*descending=*/false);
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
    apply_wire(delta.bids, bids_, /*descending=*/true);
    apply_wire(delta.asks, asks_, /*descending=*/false);
    last_id_ = delta.last_update_id;
    return settle();
}

BookResult DepthBook::seed(const WireOrderBook& wire) {
    seed_in_flight_ = false;
    if (state_ == State::Live) {
        return BookResult::Stale; // a seed outlived the book it was meant for
    }
    held_bids_.clear();
    held_asks_.clear();
    apply_wire(wire.bids, held_bids_, /*descending=*/true);
    apply_wire(wire.asks, held_asks_, /*descending=*/false);
    if (held_bids_.empty() || held_asks_.empty()) {
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
        apply_levels(pending_[i].bids, bids_, /*descending=*/true);
        apply_levels(pending_[i].asks, asks_, /*descending=*/false);
        last_id_ = pending_[i].last_id;
    }
    pending_.clear();
    return settle();
}

BookResult DepthBook::settle() {
    if (bids_.empty() || asks_.empty()) {
        // Gate maintains the window, so a side emptying means what is held no
        // longer describes the venue's book; a fresh read is the way back.
        invalidate();
        return BookResult::Reseed;
    }
    return BookResult::Applied;
}

void DepthBook::install_held() {
    bids_ = std::move(held_bids_);
    asks_ = std::move(held_asks_);
    held_bids_.clear();
    held_asks_.clear();
    last_id_ = held_id_;
    state_ = State::Live;
}

void DepthBook::discard_held() {
    held_bids_.clear();
    held_asks_.clear();
    held_id_ = 0;
    state_ = State::Cold;
    ++reseeds_;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live || bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    BookSnapshot out{};
    out.instrument = instrument_;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    out.venue_seq = last_id_;
    const auto fill = [](std::span<const Level> src, std::array<BookLevel, k_book_levels>& dst) {
        const auto n = std::min(src.size(), k_book_levels);
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = {.price = src[i].price, .qty = src[i].qty};
        }
        return static_cast<std::uint32_t>(n);
    };
    out.bid_count = fill(bids_, out.bids);
    out.ask_count = fill(asks_, out.asks);
    return out;
}

} // namespace raikiri::venues::gate
