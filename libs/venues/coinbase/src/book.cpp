#include <raikiri/venues/coinbase/book.hpp>

#include <algorithm>

namespace raikiri::venues::coinbase {

namespace {

// The same exactness rule the mappers apply. A remainder means Coinbase moved
// a product's increment under us, and rounding it would quote a price the
// venue never published.
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

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    bids_.clear();
    asks_.clear();
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    synced_ = false;
}

std::optional<Book::Level> Book::convert(const WireLevel& wire) const {
    const auto price = exact_steps(wire.price, tick_size_);
    const auto qty = exact_steps(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void Book::upsert(std::vector<Level>& side, bool descending, const Level& level) {
    const auto cmp = [descending](const Level& a, const Level& b) {
        return descending ? a.price.raw > b.price.raw : a.price.raw < b.price.raw;
    };
    const auto pos = std::lower_bound(side.begin(), side.end(), level, cmp);
    const bool exists = pos != side.end() && pos->price.raw == level.price.raw;

    if (level.qty.raw == 0) {
        // Size zero is Coinbase's delete: 1924 of 4092 live changes carry one.
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

BookResult Book::reset(const WireL2Snapshot& wire) {
    bids_.clear();
    asks_.clear();
    const auto take = [this](std::span<const WireLevel> src, std::vector<Level>& into) {
        into.reserve(src.size());
        for (const auto& entry : src) {
            const auto level = convert(entry);
            if (!level.has_value()) {
                return false;
            }
            into.push_back(*level);
        }
        return true;
    };
    // A snapshot with one level we cannot read is not one to chain onto: the
    // stream only sends changes, so the hole would never heal.
    if (!take(wire.bids, bids_) || !take(wire.asks, asks_) || bids_.empty() || asks_.empty()) {
        invalidate();
        return BookResult::BadLevel;
    }
    band_low_ = bids_.back().price;
    band_high_ = asks_.back().price;
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
        upsert(change.bid ? bids_ : asks_, /*descending=*/change.bid, *level);
    }
    if (bids_.empty() || asks_.empty()) {
        // The price walked clean out of the band, so what is left is no longer
        // the top of the venue's book.
        invalidate();
        return BookResult::BadLevel;
    }
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    const auto fill = [](std::span<const Level> src, std::array<BookLevel, k_book_levels>& dst) {
        const auto count = std::min(src.size(), k_book_levels);
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = {.price = src[i].price, .qty = src[i].qty};
        }
        return static_cast<std::uint32_t>(count);
    };

    BookSnapshot out{};
    out.instrument = instrument_;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    // Left at zero on purpose: level2_batch publishes no ordering token, and a
    // number we invented would read as one Coinbase stood behind.
    out.venue_seq = 0;
    out.bid_count = fill(bids_, out.bids);
    out.ask_count = fill(asks_, out.asks);
    return out;
}

} // namespace raikiri::venues::coinbase
