#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/map.hpp>

namespace raikiri::venues::bitfinex {

namespace {

// The venue's own reader: parse_decimal_e9 refuses the exponents this venue
// writes. The exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_number_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

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

} // namespace

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
}

bool Book::apply_level(const WireLevel& wire) {
    const auto price = steps_of(wire.price, tick_size_);
    if (!price.has_value()) {
        return false;
    }
    const auto amount = split_sign(wire.amount);
    auto& side = amount.negative ? ladders_.asks() : ladders_.bids();
    const auto price_ticks = PriceTicks::from_raw(*price);

    if (wire.count == 0) {
        // COUNT zero is the delete, and the amount beside it is a bare +1/-1
        // that says only which side to look on — never a size to rest here.
        side.remove(price_ticks);
        return true;
    }

    const auto qty = steps_of(amount.magnitude, lot_size_);
    if (!qty.has_value() || *qty == 0) {
        return false;
    }
    side.upsert(book::Level{price_ticks, QtyLots::from_raw(*qty)});
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        ladders_.clear();
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
    if (!synced_) {
        return std::nullopt;
    }
    // venue_seq left at zero on purpose: this channel publishes no ordering
    // token at all, and a number we invented would read as one Bitfinex stood
    // behind.
    return ladders_.snapshot(instrument_, /*venue_seq=*/0, ts_event, ts_init);
}

std::optional<Bbo> Book::touch(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || ladders_.bids().empty() || ladders_.asks().empty()) {
        return std::nullopt;
    }
    const auto& bid = ladders_.bids().levels().front();
    const auto& ask = ladders_.asks().levels().front();
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bid.price;
    out.ask = ask.price;
    out.bid_qty = bid.qty;
    out.ask_qty = ask.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::bitfinex
