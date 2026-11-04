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
    case Algo::Iceberg:
        return "iceberg";
    case Algo::Pegged:
        return "pegged";
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

std::string_view to_string(Refill refill) noexcept {
    return refill == Refill::OnFill ? "fill" : "complete";
}

std::string_view to_string(PegTo to) noexcept {
    switch (to) {
    case PegTo::Primary:
        return "primary";
    case PegTo::Mid:
        return "mid";
    case PegTo::Market:
        return "market";
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
    case SpecError::TagTooLong:
        return "a client tag is at most 64 characters";
    case SpecError::RefTooLong:
        return "an external reference is at most 40 characters";
    case SpecError::NoChildCap:
        return "max_child_notional must be positive";
    case SpecError::BadDepthShare:
        return "depth_share_bp is at most 10000 and depth_band_bp must be positive";
    case SpecError::BadExpiry:
        return "a resting algo's horizon is zero for none, or between one second and a week";
    case SpecError::NoRestLimit:
        return "an iceberg needs a limit";
    case SpecError::NoShow:
        return "an iceberg needs a positive show size";
    case SpecError::ShowTooBig:
        return "the show size is larger than the order";
    case SpecError::BadVariance:
        return "the show variance is at most 5000 bp";
    case SpecError::BothOffsets:
        return "give offset_ticks or offset_bp, not both";
    case SpecError::BadOffset:
        return "a peg offset is within 100000 ticks or 1000 bp";
    case SpecError::NoThreshold:
        return "the re-peg threshold is at least one tick";
    case SpecError::BadInterval:
        return "the minimum re-peg interval is between 100 ms and an hour";
    case SpecError::PostOnlyCrosses:
        return "a post-only market peg needs a positive offset, or it sits on the other side's touch";
    }
    return "unknown";
}

namespace internal {

constexpr std::uint64_t k_second = 1'000'000'000ULL;
constexpr std::uint64_t k_week = 7ULL * 24 * 60 * 60 * k_second;

[[nodiscard]] std::expected<void, SpecError> validate_resting(const ParentSpec& spec) noexcept {
    if (spec.horizon_ns != 0 && (spec.horizon_ns < k_second || spec.horizon_ns > k_week)) {
        return std::unexpected{SpecError::BadExpiry};
    }
    if (spec.algo == Algo::Iceberg) {
        const auto& iceberg = spec.iceberg;
        if (spec.limit.is_undef()) {
            return std::unexpected{SpecError::NoRestLimit};
        }
        if (iceberg.show.is_undef() || iceberg.show.raw <= 0) {
            return std::unexpected{SpecError::NoShow};
        }
        if (!spec.qty.is_undef() && spec.qty.raw > 0 && iceberg.show > spec.qty) {
            return std::unexpected{SpecError::ShowTooBig};
        }
        if (iceberg.variance_bp > 5000) {
            return std::unexpected{SpecError::BadVariance};
        }
        return {};
    }
    const auto& peg = spec.peg;
    if (peg.offset_ticks != 0 && peg.offset_bp != 0) {
        return std::unexpected{SpecError::BothOffsets};
    }
    if (peg.offset_ticks < -100'000 || peg.offset_ticks > 100'000 || peg.offset_bp < -1000 || peg.offset_bp > 1000) {
        return std::unexpected{SpecError::BadOffset};
    }
    if (peg.threshold_ticks == 0) {
        return std::unexpected{SpecError::NoThreshold};
    }
    if (peg.min_interval_ns < k_second / 10 || peg.min_interval_ns > 3600 * k_second) {
        return std::unexpected{SpecError::BadInterval};
    }
    if (spec.post_only && peg.to == PegTo::Market && peg.offset_ticks <= 0 && peg.offset_bp <= 0) {
        return std::unexpected{SpecError::PostOnlyCrosses};
    }
    return {};
}

} // namespace internal

std::expected<void, SpecError> validate(const ParentSpec& spec) noexcept {
    using internal::k_second;
    using internal::k_week;
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
    if (spec.depth_share_bp > 10'000 || !positive(spec.depth_band_bp)) {
        return std::unexpected{SpecError::BadDepthShare};
    }
    if (spec.client_tag.size() > 64) {
        return std::unexpected{SpecError::TagTooLong};
    }
    if (spec.external_ref.size() > k_external_ref_len) {
        return std::unexpected{SpecError::RefTooLong};
    }
    if (spec.algo == Algo::Sor) {
        return {};
    }
    if (resting(spec.algo)) {
        return internal::validate_resting(spec);
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
