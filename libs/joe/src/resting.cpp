#include <raikiri/joe/resting.hpp>
#include <raikiri/joe/schedule.hpp>

#include <algorithm>

namespace raikiri::joe {
namespace internal {

[[nodiscard]] const VenueBook* book_of(std::span<const VenueBook> books, InstrumentId instrument) noexcept {
    const auto it = std::ranges::find(books, instrument, &VenueBook::instrument);
    return it == books.end() || it->levels.empty() ? nullptr : &*it;
}

} // namespace internal

Quote quote_of(InstrumentId instrument, std::span<const VenueBook> bids, std::span<const VenueBook> asks) noexcept {
    const auto* bid = internal::book_of(bids, instrument);
    const auto* ask = internal::book_of(asks, instrument);
    if (bid == nullptr || ask == nullptr) {
        return {};
    }
    const auto best_bid = bid->levels.front().price;
    const auto best_ask = ask->levels.front().price;
    return {.state = best_bid >= best_ask ? BookState::Crossed : BookState::Live, .bid = best_bid, .ask = best_ask};
}

FixedE9 rest_price(Side side, FixedE9 price, const Quote& quote, FixedE9 tick, FixedE9 limit, bool post_only) noexcept {
    if (price.is_undef() || tick.is_undef() || tick.raw <= 0) {
        return FixedE9::undef();
    }
    auto out = price.raw;
    if (side == Side::Buy) {
        if (!limit.is_undef()) {
            out = std::min(out, limit.raw);
        }
        if (post_only) {
            out = std::min(out, quote.ask.raw - tick.raw);
        }
        out = out / tick.raw * tick.raw;
    } else {
        if (!limit.is_undef()) {
            out = std::max(out, limit.raw);
        }
        if (post_only) {
            out = std::max(out, quote.bid.raw + tick.raw);
        }
        out = (out + tick.raw - 1) / tick.raw * tick.raw;
    }
    return out > 0 ? FixedE9::from_raw(out) : FixedE9::undef();
}

FixedE9 peg_reference(Side side, PegTo to, const Quote& quote) noexcept {
    switch (to) {
    case PegTo::Primary:
        return side == Side::Buy ? quote.bid : quote.ask;
    case PegTo::Market:
        return side == Side::Buy ? quote.ask : quote.bid;
    case PegTo::Mid:
        break;
    }
    return FixedE9::from_raw((quote.bid.raw + quote.ask.raw) / 2);
}

FixedE9
peg_price(Side side, const PegSpec& peg, const Quote& quote, FixedE9 tick, FixedE9 limit, bool post_only) noexcept {
    if (tick.is_undef() || tick.raw <= 0) {
        return FixedE9::undef();
    }
    const auto reference = peg_reference(side, peg.to, quote);
    const auto offset = tick.raw * peg.offset_ticks + mul_div(reference.raw, peg.offset_bp, 10'000);
    const auto price = FixedE9::from_raw(side == Side::Buy ? reference.raw - offset : reference.raw + offset);
    return rest_price(side, price, quote, tick, limit, post_only);
}

std::int64_t
next_show(const IcebergSpec& iceberg, FixedE9 lot, std::int64_t left_lots, std::uint64_t& random) noexcept {
    const auto base = std::max<std::int64_t>(lots_in(iceberg.show, lot), 1);
    const auto reach = static_cast<std::uint64_t>(mul_div(base, iceberg.variance_bp, 10'000));
    auto lots = base;
    if (reach > 0) {
        const auto shift = internal::splitmix(random) % (2 * reach + 1);
        lots = base + static_cast<std::int64_t>(shift) - static_cast<std::int64_t>(reach);
    }
    return std::min(std::max<std::int64_t>(lots, 1), left_lots);
}

std::int64_t fit_lots(std::int64_t want,
                      std::int64_t left_lots,
                      FixedE9 lot,
                      FixedE9 price,
                      FixedE9 min_notional,
                      FixedE9 max_notional) noexcept {
    const auto per_lot = value_of(lot, price);
    if (per_lot.raw <= 0 || left_lots <= 0) {
        return 0;
    }
    const auto most = max_notional.raw / per_lot.raw;
    const auto least = min_notional.is_undef() || min_notional.raw <= 0
                           ? std::int64_t{1}
                           : (min_notional.raw + per_lot.raw - 1) / per_lot.raw;
    if (least > most || least > left_lots) {
        return 0;
    }
    return std::min({std::max(want, least), most, left_lots});
}

} // namespace raikiri::joe
