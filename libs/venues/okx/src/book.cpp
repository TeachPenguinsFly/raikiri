#include <raikiri/venues/okx/book.hpp>

#include <algorithm>

namespace raikiri::venues::okx {

namespace {

// Same exactness rule the mappers apply: a value that does not divide by the
// instrument's step means OKX moved a precision under us, and rounding it
// would quote a price the venue never published.
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
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {
    bids_.reserve(k_max_levels);
    asks_.reserve(k_max_levels);
}

void Book::invalidate() noexcept {
    bids_.clear();
    asks_.clear();
    synced_ = false;
    last_seq_id_ = -1;
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
            // Quantity zero is OKX's delete.
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

BookResult Book::apply(const WireBooks& update) {
    if (update.snapshot) {
        bids_.clear();
        asks_.clear();
        if (!apply_side(update.bids, bids_, /*descending=*/true) ||
            !apply_side(update.asks, asks_, /*descending=*/false)) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_seq_id_ = update.seq_id;
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // A delta before any snapshot has no book to chain onto.
        return BookResult::Ignored;
    }
    if (update.prev_seq_id != last_seq_id_) {
        if (update.seq_id <= last_seq_id_) {
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
    last_seq_id_ = update.seq_id;
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
    out.venue_seq = static_cast<std::uint64_t>(last_seq_id_);
    out.bid_count = fill(bids_, out.bids);
    out.ask_count = fill(asks_, out.asks);
    return out;
}

} // namespace raikiri::venues::okx
