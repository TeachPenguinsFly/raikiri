#include <raikiri/venues/bybit/book_sync.hpp>

#include <algorithm>

namespace raikiri::venues::bybit {

namespace {

// Venue strings convert through the decimal parser and then divide exactly
// by the instrument's step. A remainder means bybit changed a filter under
// us — dropping the level is right, silently rounding is not.
std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || step.raw <= 0) {
        return std::nullopt;
    }
    if (value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

} // namespace

BookSync::BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {
    bids_.reserve(k_max_levels);
    asks_.reserve(k_max_levels);
}

void BookSync::invalidate() noexcept {
    bids_.clear();
    asks_.clear();
    synced_ = false;
    last_update_id_ = 0;
    last_seq_ = 0;
}

bool BookSync::apply_side(std::span<const WireLevel> levels,
                          std::vector<Level>& side,
                          bool descending) {
    for (const auto& wire : levels) {
        const auto price = exact_steps(wire.price, tick_size_);
        const auto size = exact_steps(wire.size, lot_size_);
        if (!price.has_value() || !size.has_value()) {
            return false;
        }
        const Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*size)};

        const auto cmp = [descending](const Level& a, const Level& b) {
            return descending ? a.price.raw > b.price.raw : a.price.raw < b.price.raw;
        };
        const auto pos = std::lower_bound(side.begin(), side.end(), level, cmp);
        const bool exists = pos != side.end() && pos->price.raw == level.price.raw;

        if (level.size.raw == 0) {
            // Size zero is bybit's delete.
            if (exists) {
                side.erase(pos);
            }
            continue;
        }
        if (exists) {
            pos->size = level.size;
        } else {
            side.insert(pos, level);
        }
    }
    return true;
}

SyncResult BookSync::apply(const WireOrderbook& update) {
    if (update.is_snapshot) {
        bids_.clear();
        asks_.clear();
        if (!apply_side(update.bids, bids_, /*descending=*/true) ||
            !apply_side(update.asks, asks_, /*descending=*/false)) {
            invalidate();
            return SyncResult::BadLevel;
        }
        last_update_id_ = update.update_id;
        last_seq_ = update.seq;
        synced_ = true;
        return SyncResult::Resynced;
    }

    if (!synced_) {
        // A delta before any snapshot cannot be applied to anything.
        return SyncResult::Ignored;
    }
    if (update.update_id <= last_update_id_) {
        return SyncResult::Ignored; // duplicate or reordered; already applied
    }
    if (update.update_id != last_update_id_ + 1) {
        ++gaps_;
        invalidate();
        return SyncResult::GapDetected;
    }

    if (!apply_side(update.bids, bids_, /*descending=*/true) ||
        !apply_side(update.asks, asks_, /*descending=*/false)) {
        invalidate();
        return SyncResult::BadLevel;
    }
    last_update_id_ = update.update_id;
    last_seq_ = update.seq;
    return SyncResult::Applied;
}

std::optional<Bbo> BookSync::bbo(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bids_.front().price;
    out.bid_qty = bids_.front().size;
    out.ask = asks_.front().price;
    out.ask_qty = asks_.front().size;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> BookSync::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    const auto fill = [](std::span<const Level> src, std::array<BookLevel, k_book_levels>& dst) {
        const auto count = std::min(src.size(), k_book_levels);
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = {.price = src[i].price, .qty = src[i].size};
        }
        return static_cast<std::uint32_t>(count);
    };

    BookSnapshot out{};
    out.instrument = instrument_;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    out.venue_seq = last_update_id_;
    out.bid_count = fill(bids_, out.bids);
    out.ask_count = fill(asks_, out.asks);
    return out;
}

} // namespace raikiri::venues::bybit
