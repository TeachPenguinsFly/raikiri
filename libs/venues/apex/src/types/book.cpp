#include <raikiri/venues/apex/book.hpp>

namespace raikiri::venues::apex {

namespace {

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

BookSync::BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void BookSync::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    last_update_id_ = 0;
}

bool BookSync::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto price = steps_of(wire.price, tick_size_);
        const auto size = steps_of(wire.size, lot_size_);
        if (!price.has_value() || !size.has_value()) {
            return false;
        }
        const book::Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*size)};
        if (level.qty.raw == 0) {
            side.remove(level.price);
            continue;
        }
        side.upsert(level);
    }
    return true;
}

SyncResult BookSync::apply(const WireOrderbook& update) {
    if (update.is_snapshot) {
        ladders_.clear();
        if (!apply_side(update.bids, ladders_.bids()) ||
            !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            return SyncResult::BadLevel;
        }
        last_update_id_ = update.update_id;
        synced_ = true;
        return SyncResult::Resynced;
    }
    if (!synced_ || update.update_id <= last_update_id_) {
        return SyncResult::Ignored;
    }
    if (update.update_id != last_update_id_ + 1) {
        ++gaps_;
        invalidate();
        return SyncResult::GapDetected;
    }
    if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
        invalidate();
        return SyncResult::BadLevel;
    }
    last_update_id_ = update.update_id;
    return SyncResult::Applied;
}

std::optional<Bbo> BookSync::bbo(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || ladders_.bids().empty() || ladders_.asks().empty()) {
        return std::nullopt;
    }
    const auto& bid = ladders_.bids().levels().front();
    const auto& ask = ladders_.asks().levels().front();
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bid.price;
    out.bid_qty = bid.qty;
    out.ask = ask.price;
    out.ask_qty = ask.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> BookSync::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_update_id_, ts_event, ts_init);
}

} // namespace raikiri::venues::apex
