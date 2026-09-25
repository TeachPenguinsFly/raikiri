#include <raikiri/venues/grvt/book.hpp>
#include <raikiri/venues/grvt/map.hpp>

namespace raikiri::venues::grvt {

Step Sequence::offer(std::uint64_t sequence, std::uint64_t prev) noexcept {
    if (sequence == 0) {
        // A snapshot with no ack before it has nothing to chain to.
        if (!anchored_) {
            ++gaps_;
            drop();
            return Step::Gap;
        }
        last_ = anchor_;
        anchored_ = false;
        live_ = true;
        asked_ = false;
        return Step::Snapshot;
    }
    if (!live_) {
        return Step::Cold;
    }
    if (prev == last_) {
        last_ = sequence;
        return Step::Apply;
    }
    if (sequence <= last_) {
        return Step::Stale;
    }
    ++gaps_;
    drop();
    return Step::Gap;
}

bool Sequence::take_resubscribe() noexcept {
    if (asked_) {
        return false;
    }
    asked_ = true;
    return true;
}

void Sequence::drop() noexcept {
    live_ = false;
}

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {
    ladders_.bids().reserve(k_book_levels * 4);
    ladders_.asks().reserve(k_book_levels * 4);
}

bool DepthBook::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& level : levels) {
        const auto price = to_ticks(level.price, tick_size_);
        const auto size = parse_decimal_e9(level.size);
        if (!price.has_value() || !size.has_value()) {
            return false;
        }
        if (size->raw == 0) {
            side.remove(*price);
            continue;
        }
        const auto lots = book::exact_steps(*size, lot_size_);
        if (!lots.has_value()) {
            return false;
        }
        side.upsert({.price = *price, .qty = QtyLots::from_raw(*lots)});
    }
    return true;
}

bool DepthBook::replace(const WireBook& snapshot) {
    ladders_.clear();
    return apply(snapshot);
}

bool DepthBook::apply(const WireBook& delta) {
    if (apply_side(delta.bids, ladders_.bids()) && apply_side(delta.asks, ladders_.asks())) {
        return true;
    }
    ladders_.clear();
    sequence.drop();
    return false;
}

std::optional<BookSnapshot> DepthBook::snapshot(std::uint64_t venue_seq, UnixNanos ts_event, UnixNanos ts_init) const {
    if (!sequence.live()) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, venue_seq, ts_event, ts_init);
}

Quote::Quote(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Quote::clear() noexcept {
    bid_ = PriceTicks::undef();
    ask_ = PriceTicks::undef();
    bid_qty_ = QtyLots::undef();
    ask_qty_ = QtyLots::undef();
}

bool Quote::replace(const WireQuote& snapshot) {
    clear();
    return apply(snapshot);
}

bool Quote::apply(const WireQuote& delta) {
    const auto merge_price = [this](const std::optional<std::string_view>& text, PriceTicks& into) {
        if (!text.has_value()) {
            return true;
        }
        if (text->empty()) {
            into = PriceTicks::undef();
            return true;
        }
        const auto ticks = to_ticks(*text, tick_size_);
        if (!ticks.has_value()) {
            return false;
        }
        into = *ticks;
        return true;
    };
    const auto merge_size = [this](const std::optional<std::string_view>& text, QtyLots& into) {
        if (!text.has_value()) {
            return true;
        }
        if (text->empty()) {
            into = QtyLots::undef();
            return true;
        }
        const auto lots = to_lots(*text, lot_size_);
        if (!lots.has_value()) {
            return false;
        }
        into = *lots;
        return true;
    };
    if (merge_price(delta.bid_price, bid_) && merge_size(delta.bid_size, bid_qty_) &&
        merge_price(delta.ask_price, ask_) && merge_size(delta.ask_size, ask_qty_)) {
        return true;
    }
    clear();
    sequence.drop();
    return false;
}

std::optional<Bbo> Quote::bbo(UnixNanos ts_event, UnixNanos ts_init) const noexcept {
    if (!sequence.live() || bid_.is_undef() || ask_.is_undef() || bid_qty_.is_undef() || ask_qty_.is_undef()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bid_;
    out.ask = ask_;
    out.bid_qty = bid_qty_;
    out.ask_qty = ask_qty_;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::grvt
