#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/kucoin/book.hpp>

#include <utility>

namespace raikiri::venues::kucoin {

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
    held_sequence_ = 0;
    last_sequence_ = 0;
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    state_ = State::Cold;
    seed_in_flight_ = false;
}

std::optional<DepthBook::Level> DepthBook::convert(std::string_view price,
                                                   std::string_view qty) const {
    const auto ticks = steps_of(price, tick_size_);
    const auto lots = steps_of(qty, lot_size_);
    if (!ticks.has_value() || !lots.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*ticks), .qty = QtyLots::from_raw(*lots)};
}

void DepthBook::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        // Size zero is KuCoin's delete: 1914 of 6804 live frames carry one.
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

void DepthBook::place(const Level& level, bool bid) {
    upsert(bid ? ladders_.bids() : ladders_.asks(), level);
}

BookResult DepthBook::apply(const WireBookChange& change) {
    const auto level = convert(change.price, change.qty);
    if (!level.has_value()) {
        // A level we cannot convert is not one we may skip: from here the book
        // would disagree with KuCoin's permanently, and invisibly.
        invalidate();
        return BookResult::BadLevel;
    }
    switch (state_) {
    case State::Cold:
        return buffer(change.sequence, *level, change.bid);
    case State::Bridging:
        return bridge(change.sequence, *level, change.bid);
    case State::Live:
        return advance(change.sequence, *level, change.bid);
    }
    return BookResult::Waiting;
}

BookResult DepthBook::buffer(std::int64_t sequence, const Level& level, bool bid) {
    if (!pending_.empty() && sequence != pending_.back().sequence + 1) {
        pending_.clear(); // the chain broke before the seed landed
    }
    if (pending_.size() == k_max_buffered_changes) {
        pending_.clear();
    }
    pending_.push_back(Buffered{.sequence = sequence, .level = level, .bid = bid});
    return BookResult::Buffered;
}

BookResult DepthBook::bridge(std::int64_t sequence, const Level& level, bool bid) {
    const auto wanted = held_sequence_ + 1;
    if (sequence < wanted) {
        return BookResult::Stale; // this change is already in the seed
    }
    if (sequence > wanted) {
        // The chain is contiguous, so nothing will ever fill what is missing
        // between the seed and here.
        discard_held();
        return BookResult::Reseed;
    }
    install_held();
    place(level, bid);
    last_sequence_ = sequence;
    return settle();
}

BookResult DepthBook::advance(std::int64_t sequence, const Level& level, bool bid) {
    if (sequence <= last_sequence_) {
        return BookResult::Stale; // duplicate or reordered; already applied
    }
    if (sequence != last_sequence_ + 1) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    place(level, bid);
    last_sequence_ = sequence;
    return settle();
}

BookResult DepthBook::seed(const WireOrderBook& wire) {
    seed_in_flight_ = false;
    if (state_ == State::Live) {
        return BookResult::Stale; // a seed outlived the book it was meant for
    }
    held_.clear();
    const auto take = [this](std::span<const WireLevel> src, book::Ladder& into) {
        into.reserve(src.size());
        for (const auto& entry : src) {
            const auto level = convert(entry.price, entry.qty);
            if (!level.has_value()) {
                return false;
            }
            into.upsert(*level);
        }
        return true;
    };
    // A seed with one level we cannot read is not a seed we can chain onto,
    // and a hole in it would never heal — the stream only sends changes.
    if (!take(wire.bids, held_.bids()) || !take(wire.asks, held_.asks()) || held_.bids().empty() ||
        held_.asks().empty()) {
        discard_held();
        return BookResult::Reseed;
    }
    held_sequence_ = wire.sequence;
    band_low_ = held_.bids().levels().back().price;
    band_high_ = held_.asks().levels().back().price;
    state_ = State::Bridging;
    return replay();
}

BookResult DepthBook::replay() {
    const auto wanted = held_sequence_ + 1;
    std::size_t i = 0;
    while (i < pending_.size() && pending_[i].sequence < wanted) {
        ++i;
    }
    if (i == pending_.size()) {
        pending_.clear();
        return BookResult::Waiting; // the seed is newer than anything buffered
    }
    if (pending_[i].sequence > wanted) {
        discard_held();
        return BookResult::Reseed;
    }

    install_held();
    // buffer() already enforced contiguity across pending_, so the replay only
    // has to find where the seed sits inside it.
    for (; i < pending_.size(); ++i) {
        place(pending_[i].level, pending_[i].bid);
        last_sequence_ = pending_[i].sequence;
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
    last_sequence_ = held_sequence_;
    state_ = State::Live;
}

void DepthBook::discard_held() {
    held_.clear();
    held_sequence_ = 0;
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    state_ = State::Cold;
    ++reseeds_;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live) {
        return std::nullopt;
    }
    return ladders_.snapshot(
        instrument_, static_cast<std::uint64_t>(last_sequence_), ts_event, ts_init);
}

} // namespace raikiri::venues::kucoin
