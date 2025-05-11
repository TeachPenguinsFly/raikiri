#include <raikiri/venues/bitmex/book.hpp>
#include <raikiri/venues/bitmex/map.hpp>

#include <algorithm>

namespace raikiri::venues::bitmex {

namespace {

// The venue's own reader: BitMEX writes one field as "0.0001" on REST and
// "1.0E-4" on the socket, so the exponent form has to survive. The
// exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_number_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    bid_rows_.clear();
    ask_rows_.clear();
    synced_ = false;
}

book::Ladder& Book::side_of(const WireLevel& row) noexcept {
    return row.bid ? ladders_.bids() : ladders_.asks();
}

std::vector<Book::Row>& Book::rows_of(const WireLevel& row) noexcept {
    return row.bid ? bid_rows_ : ask_rows_;
}

bool Book::upsert(const WireLevel& row) {
    const auto price = steps_of(row.price, tick_size_);
    const auto qty = steps_of(row.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return false;
    }
    const auto at = PriceTicks::from_raw(*price);
    // An insert at a price already held replaces it, id and all, the way one
    // row set keyed on both would.
    auto& rows = rows_of(row);
    std::erase_if(rows, [at](const Row& held) { return held.price.raw == at.raw; });
    rows.push_back(Row{.id = row.id, .price = at});
    side_of(row).upsert(Level{.price = at, .qty = QtyLots::from_raw(*qty)});
    return true;
}

BookResult Book::amend(const WireLevel& row) {
    auto& rows = rows_of(row);
    const auto at =
        std::ranges::find_if(rows, [&row](const Row& held) { return held.id == row.id; });
    if (at == rows.end()) {
        // An update names a level the venue believes we hold. Not holding it
        // means a frame went missing, and this table has no sequence that
        // would have said so — this is the only place divergence surfaces.
        return BookResult::Diverged;
    }
    if (row.qty.empty()) {
        return BookResult::Applied; // `update` sends only what changed
    }
    const auto qty = steps_of(row.qty, lot_size_);
    if (!qty.has_value()) {
        return BookResult::BadLevel;
    }
    side_of(row).upsert(Level{.price = at->price, .qty = QtyLots::from_raw(*qty)});
    return BookResult::Applied;
}

void Book::erase(const WireLevel& row) {
    auto& rows = rows_of(row);
    const auto at =
        std::ranges::find_if(rows, [&row](const Row& held) { return held.id == row.id; });
    if (at == rows.end()) {
        ++stale_deletes_;
        return;
    }
    side_of(row).remove(at->price);
    rows.erase(at);
}

BookResult Book::apply(FrameAction action, std::span<const WireLevel> rows) {
    if (action == FrameAction::Partial) {
        ladders_.clear();
        bid_rows_.clear();
        ask_rows_.clear();
        for (const auto& row : rows) {
            if (!upsert(row)) {
                invalidate();
                return BookResult::BadLevel;
            }
        }
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // Rows before any partial have no book to key into.
        return BookResult::Ignored;
    }

    for (const auto& row : rows) {
        switch (action) {
        case FrameAction::Insert:
            if (!upsert(row)) {
                invalidate();
                return BookResult::BadLevel;
            }
            break;
        case FrameAction::Update:
            if (const auto result = amend(row); result != BookResult::Applied) {
                if (result == BookResult::Diverged) {
                    ++divergences_;
                }
                invalidate();
                return result;
            }
            break;
        case FrameAction::Delete:
            erase(row);
            break;
        case FrameAction::Partial:
        case FrameAction::None:
            return BookResult::Ignored;
        }
    }
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    // The sequence is left at zero on purpose: BitMEX publishes none on this
    // table, and a number we invented would read downstream as one the venue
    // stood behind.
    return ladders_.snapshot(instrument_, 0, ts_event, ts_init);
}

} // namespace raikiri::venues::bitmex
