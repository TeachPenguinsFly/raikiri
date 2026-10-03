#include <raikiri/options/convention.hpp>

namespace raikiri::options {

namespace {

[[nodiscard]] FixedE9 divided(FixedE9 value, std::int64_t by) noexcept {
    return value.is_undef() ? value : FixedE9::from_raw(value.raw / by);
}

void put(const std::optional<FixedE9>& from, FixedE9& into) noexcept {
    if (from.has_value()) {
        into = *from;
    }
}

} // namespace

FixedE9 to_fraction(FixedE9 iv, IvScale scale) noexcept {
    return scale == IvScale::Percent ? divided(iv, 100) : iv;
}

FixedE9 to_per_vol_point(FixedE9 vega, VegaScale scale) noexcept {
    return scale == VegaScale::PerUnitVol ? divided(vega, 100) : vega;
}

FixedE9 to_per_day(FixedE9 theta, ThetaScale scale) noexcept {
    return scale == ThetaScale::PerYear ? divided(theta, 365) : theta;
}

OptionGreeks normalise(InstrumentId instrument,
                       const VenueGreeks& venue,
                       Convention convention,
                       UnixNanos ts_event,
                       UnixNanos ts_init) noexcept {
    OptionGreeks out{};
    out.instrument = instrument;
    put(venue.delta, out.delta);
    put(venue.gamma, out.gamma);
    put(venue.rho, out.rho);
    put(venue.underlying_price, out.underlying_price);
    put(venue.mark_price, out.mark_price);
    put(venue.index_price, out.index_price);
    if (venue.vega.has_value()) {
        out.vega = to_per_vol_point(*venue.vega, convention.vega);
    }
    if (venue.theta.has_value()) {
        out.theta = to_per_day(*venue.theta, convention.theta);
    }
    if (venue.mark_iv.has_value()) {
        out.mark_iv = to_fraction(*venue.mark_iv, convention.iv);
    }
    if (venue.bid_iv.has_value()) {
        out.bid_iv = to_fraction(*venue.bid_iv, convention.iv);
    }
    if (venue.ask_iv.has_value()) {
        out.ask_iv = to_fraction(*venue.ask_iv, convention.iv);
    }
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::options
