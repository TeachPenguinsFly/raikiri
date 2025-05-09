#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>

#include <algorithm>

namespace raikiri::book {

namespace {

[[nodiscard]] auto comparator(Side side) noexcept {
    return [side](const Level& a, const Level& b) {
        return side == Side::Bid ? a.price.raw > b.price.raw : a.price.raw < b.price.raw;
    };
}

} // namespace

void Ladder::upsert(Level level) {
    const auto pos = std::lower_bound(levels_.begin(), levels_.end(), level, comparator(side_));
    if (pos != levels_.end() && pos->price.raw == level.price.raw) {
        pos->qty = level.qty;
        return;
    }
    levels_.insert(pos, level);
}

void Ladder::remove(PriceTicks price) {
    const Level probe{price, QtyLots{}};
    const auto pos = std::lower_bound(levels_.begin(), levels_.end(), probe, comparator(side_));
    if (pos != levels_.end() && pos->price.raw == price.raw) {
        levels_.erase(pos);
    }
}

void Ladder::truncate(std::size_t depth) {
    if (levels_.size() > depth) {
        levels_.resize(depth);
    }
}

void Ladder::drop_beyond(PriceTicks worst) {
    const auto keep = std::find_if(levels_.begin(), levels_.end(), [&](const Level& level) {
        return precedes(worst, level.price);
    });
    levels_.erase(keep, levels_.end());
}

std::optional<BookSnapshot> Ladders::snapshot(InstrumentId instrument,
                                              std::uint64_t venue_seq,
                                              UnixNanos ts_event,
                                              UnixNanos ts_init) const {
    if (bids_.empty() || asks_.empty()) {
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
    out.instrument = instrument;
    out.venue_seq = venue_seq;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    out.bid_count = fill(bids_.levels(), out.bids);
    out.ask_count = fill(asks_.levels(), out.asks);
    return out;
}

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

} // namespace raikiri::book
