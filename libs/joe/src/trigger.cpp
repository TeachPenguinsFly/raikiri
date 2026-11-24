#include <raikiri/joe/trigger.hpp>

#include <algorithm>

namespace raikiri::joe {

bool fires_up(const ParentSpec& spec) noexcept {
    const bool buy = spec.side == Side::Buy;
    return spec.algo == Algo::TakeProfit ? !buy : buy;
}

FixedE9 trigger_level(const ParentSpec& spec, const TriggerState& state) noexcept {
    if (spec.algo != Algo::TrailingStop) {
        return spec.trigger.price;
    }
    if (!state.active || state.mark.is_undef()) {
        return FixedE9::undef();
    }
    const auto back = mul_div(state.mark.raw, spec.trigger.callback_bp, 10'000);
    return FixedE9::from_raw(fires_up(spec) ? state.mark.raw + back : state.mark.raw - back);
}

bool observe(const ParentSpec& spec, TriggerState& state, FixedE9 price, UnixNanos now) noexcept {
    if (state.fired.ns != 0 || price.is_undef() || price.raw <= 0) {
        return false;
    }
    const bool up = fires_up(spec);
    if (spec.algo == Algo::TrailingStop) {
        if (!state.active) {
            const auto wait = spec.trigger.activation;
            if (!wait.is_undef() && (up ? price.raw > wait.raw : price.raw < wait.raw)) {
                return false;
            }
            state.active = true;
            state.mark = price;
            return false;
        }
        state.mark = FixedE9::from_raw(up ? std::min(state.mark.raw, price.raw) : std::max(state.mark.raw, price.raw));
    }
    state.active = true;
    const auto level = trigger_level(spec, state);
    if (level.is_undef() || (up ? price.raw < level.raw : price.raw > level.raw)) {
        return false;
    }
    state.fired = now;
    state.price = price;
    return true;
}

FixedE9 trigger_mid(std::span<const VenueBook> bids, std::span<const VenueBook> asks, VenueId venue) noexcept {
    auto bid = FixedE9::undef();
    auto ask = FixedE9::undef();
    for (const auto& book : asks) {
        if (venue != VenueId::Invalid && book.instrument.venue() != venue) {
            continue;
        }
        const auto quote = quote_of(book.instrument, bids, asks);
        if (quote.state != BookState::Live) {
            continue;
        }
        if (bid.is_undef() || quote.bid.raw > bid.raw) {
            bid = quote.bid;
        }
        if (ask.is_undef() || quote.ask.raw < ask.raw) {
            ask = quote.ask;
        }
    }
    if (bid.is_undef() || ask.is_undef()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw((bid.raw + ask.raw) / 2);
}

} // namespace raikiri::joe
