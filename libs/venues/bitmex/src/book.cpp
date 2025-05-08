#include <raikiri/venues/bitmex/book.hpp>
#include <raikiri/venues/bitmex/map.hpp>

#include <algorithm>

namespace raikiri::venues::bitmex {

namespace {

// The same exactness rule the mappers apply, through the same reader: BitMEX
// writes one field as "0.0001" on REST and "1.0E-4" on the socket, so the
// exponent form has to survive. A remainder means BitMEX moved a precision
// under us, and rounding it would quote a price the venue never published.
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
}

std::vector<Book::Level>& Book::side_of(const WireLevel& row) noexcept {
    return row.bid ? bids_ : asks_;
}

bool Book::upsert(const WireLevel& row) {
    const auto price = exact_steps(row.price, tick_size_);
    const auto qty = exact_steps(row.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return false;
    }
    auto& side = side_of(row);
    const auto descending = row.bid;
    const Level level{row.id, PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};

    const auto cmp = [descending](const Level& a, const Level& b) {
        return descending ? a.price.raw > b.price.raw : a.price.raw < b.price.raw;
    };
    const auto pos = std::lower_bound(side.begin(), side.end(), level, cmp);
    if (pos != side.end() && pos->price.raw == level.price.raw) {
        *pos = level;
        return true;
    }
    side.insert(pos, level);
    return true;
}

BookResult Book::amend(const WireLevel& row) {
    auto& side = side_of(row);
    const auto at =
        std::ranges::find_if(side, [&row](const Level& level) { return level.id == row.id; });
    if (at == side.end()) {
        // An update names a level the venue believes we hold. Not holding it
        // means a frame went missing, and this table has no sequence that
        // would have said so — this is the only place divergence surfaces.
        return BookResult::Diverged;
    }
    if (row.qty.empty()) {
        return BookResult::Applied; // `update` sends only what changed
    }
    const auto qty = exact_steps(row.qty, lot_size_);
    if (!qty.has_value()) {
        return BookResult::BadLevel;
    }
    at->qty = QtyLots::from_raw(*qty);
    return BookResult::Applied;
}

void Book::erase(const WireLevel& row) {
    auto& side = side_of(row);
    const auto at =
        std::ranges::find_if(side, [&row](const Level& level) { return level.id == row.id; });
    if (at == side.end()) {
        ++stale_deletes_;
        return;
    }
    side.erase(at);
}

BookResult Book::apply(FrameAction action, std::span<const WireLevel> rows) {
    if (action == FrameAction::Partial) {
        bids_.clear();
        asks_.clear();
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
    // Left at zero on purpose: BitMEX publishes no sequence on this table, and
    // a number we invented would read downstream as one the venue stood behind.
    out.venue_seq = 0;
    out.bid_count = fill(bids_, out.bids);
    out.ask_count = fill(asks_, out.asks);
    return out;
}

} // namespace raikiri::venues::bitmex
