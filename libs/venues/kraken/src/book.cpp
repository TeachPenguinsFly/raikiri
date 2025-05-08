#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/map.hpp>

#include <algorithm>

namespace raikiri::venues::kraken {

namespace {

// The same exactness rule the mappers apply, through the same reader: Kraken
// writes prices and sizes as JSON numbers and spells one field two ways, so
// parse_number_e9 rather than parse_decimal_e9. A remainder means Kraken
// moved a precision under us, and rounding it would quote a price the venue
// never published.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(text);
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
    synced_ = false;
    last_seq_ = 0;
}

bool Book::apply_side(std::span<const WireLevel> levels,
                      std::vector<Level>& side,
                      bool descending) {
    for (const auto& wire : levels) {
        const auto price = exact_steps(wire.price, tick_size_);
        const auto qty = exact_steps(wire.qty, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        const Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};

        const auto cmp = [descending](const Level& a, const Level& b) {
            return descending ? a.price.raw > b.price.raw : a.price.raw < b.price.raw;
        };
        const auto pos = std::lower_bound(side.begin(), side.end(), level, cmp);
        const bool exists = pos != side.end() && pos->price.raw == level.price.raw;

        if (level.qty.raw == 0) {
            // Quantity zero is Kraken's delete: 615 of 8069 live deltas.
            if (exists) {
                side.erase(pos);
            }
            continue;
        }
        if (exists) {
            pos->qty = level.qty;
        } else {
            side.insert(pos, level);
        }
    }
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        bids_.clear();
        asks_.clear();
        if (!apply_side(update.bids, bids_, /*descending=*/true) ||
            !apply_side(update.asks, asks_, /*descending=*/false)) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_seq_ = update.seq;
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // A delta before any snapshot has no book to chain onto.
        return BookResult::Ignored;
    }
    if (update.seq != last_seq_ + 1) {
        if (update.seq <= last_seq_) {
            return BookResult::Ignored; // redelivery of a frame already applied
        }
        ++gaps_;
        invalidate();
        return BookResult::GapDetected;
    }

    if (!apply_side(update.bids, bids_, /*descending=*/true) ||
        !apply_side(update.asks, asks_, /*descending=*/false)) {
        invalidate();
        return BookResult::BadLevel;
    }
    last_seq_ = update.seq;
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
    out.venue_seq = last_seq_;
    out.bid_count = fill(bids_, out.bids);
    out.ask_count = fill(asks_, out.asks);
    return out;
}

} // namespace raikiri::venues::kraken
