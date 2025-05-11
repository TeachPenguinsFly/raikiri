#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/coinbase/book.hpp>

namespace raikiri::venues::coinbase {

namespace {

// The venue's own reader; the exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    synced_ = false;
}

std::optional<Book::Level> Book::convert(const WireLevel& wire) const {
    const auto price = steps_of(wire.price, tick_size_);
    const auto qty = steps_of(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void Book::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        // Size zero is Coinbase's delete: 1924 of 4092 live changes carry one.
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

BookResult Book::reset(const WireL2Snapshot& wire) {
    ladders_.clear();
    const auto take = [this](std::span<const WireLevel> src, book::Ladder& into) {
        into.reserve(src.size());
        for (const auto& entry : src) {
            const auto level = convert(entry);
            if (!level.has_value()) {
                return false;
            }
            into.upsert(*level);
        }
        return true;
    };
    // A snapshot with one level we cannot read is not one to chain onto: the
    // stream only sends changes, so the hole would never heal.
    if (!take(wire.bids, ladders_.bids()) || !take(wire.asks, ladders_.asks()) ||
        ladders_.bids().empty() || ladders_.asks().empty()) {
        invalidate();
        return BookResult::BadLevel;
    }
    band_low_ = ladders_.bids().levels().back().price;
    band_high_ = ladders_.asks().levels().back().price;
    synced_ = true;
    return BookResult::Resynced;
}

BookResult Book::apply(const WireL2Update& update) {
    if (!synced_) {
        // Changes before the snapshot have no book to change.
        return BookResult::Ignored;
    }
    for (const auto& change : update.changes) {
        const auto level = convert({.price = change.price, .qty = change.qty});
        if (!level.has_value()) {
            // A level we cannot convert is not one we may skip: from here the
            // book would disagree with Coinbase's permanently, and invisibly.
            invalidate();
            return BookResult::BadLevel;
        }
        if (change.bid ? level->price.raw < band_low_.raw : level->price.raw > band_high_.raw) {
            ++outside_band_;
            continue;
        }
        upsert(change.bid ? ladders_.bids() : ladders_.asks(), *level);
    }
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // The price walked clean out of the band, so what is left is no longer
        // the top of the venue's book.
        invalidate();
        return BookResult::BadLevel;
    }
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    // The sequence is left at zero on purpose: level2_batch publishes no
    // ordering token, and a number we invented would read as one Coinbase
    // stood behind.
    return ladders_.snapshot(instrument_, 0, ts_event, ts_init);
}

} // namespace raikiri::venues::coinbase
