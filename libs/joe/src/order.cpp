#include <raikiri/joe/order.hpp>

#include <limits>

namespace raikiri::joe {

std::string_view to_string(Algo algo) noexcept {
    switch (algo) {
    case Algo::Sor:
        return "sor";
    case Algo::Twap:
        return "twap";
    case Algo::Vwap:
        return "vwap";
    case Algo::Pov:
        return "pov";
    }
    return "unknown";
}

std::string_view to_string(Profile profile) noexcept {
    switch (profile) {
    case Profile::Flat:
        return "flat";
    case Profile::U:
        return "u";
    case Profile::Recorded:
        return "recorded";
    }
    return "unknown";
}

std::string_view to_string(Status status) noexcept {
    switch (status) {
    case Status::Pending:
        return "pending";
    case Status::Working:
        return "working";
    case Status::Paused:
        return "paused";
    case Status::Done:
        return "done";
    case Status::Cancelled:
        return "cancelled";
    case Status::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view to_string(SpecError error) noexcept {
    switch (error) {
    case SpecError::NoSymbol:
        return "no symbol";
    case SpecError::NoSize:
        return "neither qty nor notional is a positive number";
    case SpecError::BothSizes:
        return "give qty or notional, not both";
    case SpecError::BadLimit:
        return "the limit is not a positive price";
    case SpecError::NoSlices:
        return "slices must be between 1 and 10000";
    case SpecError::NoHorizon:
        return "a time-sliced algo needs a horizon of at least one second and at most a week";
    case SpecError::NoRate:
        return "pov needs a participation rate between 1 and 5000 bp";
    case SpecError::NoChildCap:
        return "max_child_notional must be positive";
    }
    return "unknown";
}

std::expected<void, SpecError> validate(const ParentSpec& spec) noexcept {
    constexpr std::uint64_t k_second = 1'000'000'000ULL;
    constexpr std::uint64_t k_week = 7ULL * 24 * 60 * 60 * k_second;
    const auto positive = [](FixedE9 v) { return !v.is_undef() && v.raw > 0; };
    if (spec.listing.symbol.view().empty()) {
        return std::unexpected{SpecError::NoSymbol};
    }
    if (positive(spec.qty) && positive(spec.notional)) {
        return std::unexpected{SpecError::BothSizes};
    }
    if (!positive(spec.qty) && !positive(spec.notional)) {
        return std::unexpected{SpecError::NoSize};
    }
    if (!spec.limit.is_undef() && spec.limit.raw <= 0) {
        return std::unexpected{SpecError::BadLimit};
    }
    if (!positive(spec.max_child_notional)) {
        return std::unexpected{SpecError::NoChildCap};
    }
    if (spec.algo == Algo::Sor) {
        return {};
    }
    if (spec.horizon_ns < k_second || spec.horizon_ns > k_week) {
        return std::unexpected{SpecError::NoHorizon};
    }
    if (spec.algo == Algo::Pov) {
        if (spec.pov_bp == 0 || spec.pov_bp > 5000) {
            return std::unexpected{SpecError::NoRate};
        }
        return {};
    }
    if (spec.slices == 0 || spec.slices > 10'000) {
        return std::unexpected{SpecError::NoSlices};
    }
    return {};
}

FixedE9 value_of(FixedE9 qty, FixedE9 price) noexcept {
    const int128_t product = static_cast<int128_t>(qty.raw) * price.raw / k_e9;
    constexpr auto k_max = std::numeric_limits<std::int64_t>::max();
    if (product > k_max) {
        return FixedE9::from_raw(k_max);
    }
    if (product < -k_max) {
        return FixedE9::from_raw(-k_max);
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(product));
}

std::int64_t lots_in(FixedE9 qty, FixedE9 lot) noexcept {
    if (qty.is_undef() || lot.is_undef() || lot.raw <= 0) {
        return 0;
    }
    return qty.raw / lot.raw;
}

FixedE9 round_down(FixedE9 qty, FixedE9 lot) noexcept {
    return FixedE9::from_raw(lots_in(qty, lot) * (lot.raw > 0 ? lot.raw : 0));
}

} // namespace raikiri::joe
