#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/map.hpp>

#include <algorithm>

namespace raikiri::venues::bitfinex {

namespace {

// The sign is the side here, never part of the quantity, so it comes off
// before anything is scaled. A negative reaching QtyLots would be a size no
// consumer could interpret.
struct SignedToken {
    std::string_view magnitude;
    bool negative{false};
};

SignedToken split_sign(std::string_view text) noexcept {
    SignedToken out{.magnitude = text, .negative = false};
    if (!out.magnitude.empty() && (out.magnitude.front() == '-' || out.magnitude.front() == '+')) {
        out.negative = out.magnitude.front() == '-';
        out.magnitude.remove_prefix(1);
    }
    return out;
}

// The same exactness rule the mappers apply, through the same reader:
// Bitfinex spells small amounts `1e-8` on the live socket, which
// parse_decimal_e9 refuses outright. A remainder means a token finer than the
// eight-decimal grid the venue is measured to hold to, and rounding it would
// quote a price the venue never published.
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

bool Book::apply_level(const WireLevel& wire) {
    const auto price = exact_steps(wire.price, tick_size_);
    if (!price.has_value()) {
        return false;
    }
    const auto amount = split_sign(wire.amount);
    auto& side = amount.negative ? asks_ : bids_;
    const auto descending = !amount.negative;

    const auto cmp = [descending](const Level& a, const Level& b) {
        return descending ? a.price.raw > b.price.raw : a.price.raw < b.price.raw;
    };
    const Level probe{PriceTicks::from_raw(*price), QtyLots{}};
    const auto pos = std::lower_bound(side.begin(), side.end(), probe, cmp);
    const bool exists = pos != side.end() && pos->price.raw == probe.price.raw;

    if (wire.count == 0) {
        // COUNT zero is the delete, and the amount beside it is a bare +1/-1
        // that says only which side to look on — never a size to rest here.
        if (exists) {
            side.erase(pos);
        }
        return true;
    }

    const auto qty = exact_steps(amount.magnitude, lot_size_);
    if (!qty.has_value() || *qty == 0) {
        return false;
    }
    const Level level{probe.price, QtyLots::from_raw(*qty)};
    if (exists) {
        *pos = level;
    } else {
        side.insert(pos, level);
    }
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        bids_.clear();
        asks_.clear();
        for (const auto& wire : update.levels) {
            if (!apply_level(wire)) {
                invalidate();
                return BookResult::BadLevel;
            }
        }
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // A delta before the opening snapshot has no book to change.
        return BookResult::Ignored;
    }
    for (const auto& wire : update.levels) {
        if (!apply_level(wire)) {
            invalidate();
            return BookResult::BadLevel;
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
    // Left at zero on purpose: this channel publishes no ordering token at
    // all, and a number we invented would read as one Bitfinex stood behind.
    out.venue_seq = 0;
    out.bid_count = fill(bids_, out.bids);
    out.ask_count = fill(asks_, out.asks);
    return out;
}

std::optional<Bbo> Book::touch(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bids_.front().price;
    out.ask = asks_.front().price;
    out.bid_qty = bids_.front().qty;
    out.ask_qty = asks_.front().qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::bitfinex
