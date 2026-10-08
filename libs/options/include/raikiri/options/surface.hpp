#pragma once

// doubles stay inside surface.cpp; vols are fractions (0.01 is one vol point), k is ln(K/F), w is σ²T

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace raikiri::options {

// a coin premium turns into usd at the venue's own forward, and its iv is implied on that forward
struct MarketQuote {
    VenueId venue{VenueId::Invalid};
    FixedE9 strike{};
    PutCall put_call{PutCall::Call};
    FixedE9 bid{};
    FixedE9 ask{};
    OptionUnit unit{OptionUnit::Quote};
    FixedE9 forward{};
    UnixNanos ts{}; // zero is never stale
};

struct SliceInput {
    UnixNanos expiry{};
    FixedE9 forward{}; // undef takes the median of the quotes' forwards
    std::vector<MarketQuote> quotes{};
};

enum class FitWeight : std::uint8_t { Spread = 0, Vega };

struct SurfaceConfig {
    FitWeight weight{FitWeight::Spread};
    FixedE9 max_spread{FixedE9::from_raw(150'000'000)}; // in vol, wider is dropped
    FixedE9 min_spread{FixedE9::from_raw(5'000'000)};   // floor of the spread weight
    FixedE9 min_delta{FixedE9::from_raw(20'000'000)};   // |Δ| at mid iv, smaller is dropped
    std::uint64_t stale_ns{60'000'000'000ULL};
    std::uint64_t min_expiry_ns{1'800'000'000'000ULL};
    std::uint32_t min_quotes{5};
    FixedE9 ssvi_tolerance{FixedE9::from_raw(5'000'000)}; // ssvi wins while its rmse is within this of svi's
};

// raw svi: w(k) = a + b(ρ(k − m) + √((k − m)² + σ²))
struct SviParams {
    FixedE9 a{};
    FixedE9 b{};
    FixedE9 rho{};
    FixedE9 m{};
    FixedE9 sigma{};
};

// w(k, θ) = θ/2 (1 + ρφk + √((φk + ρ)² + 1 − ρ²)) with φ(θ) = η / (θ^γ (1 + θ)^(1 − γ))
struct SsviParams {
    FixedE9 rho{};
    FixedE9 eta{};
    FixedE9 gamma{};
};

enum class SurfaceModel : std::uint8_t { None = 0, Ssvi, Svi };
enum class SurfaceSource : std::uint8_t { None = 0, Fitted, Borrowed, Realized };

struct SliceFit {
    UnixNanos expiry{};
    FixedE9 years{};
    FixedE9 forward{};
    FixedE9 theta{}; // atm total variance under ssvi
    SviParams svi{};
    FixedE9 k_low{};
    FixedE9 k_high{};
    std::uint32_t quotes{0};
    std::uint32_t dropped{0}; // stale, crossed, one-sided, no iv, too wide or too far out
    FixedE9 svi_rmse{};
    FixedE9 ssvi_rmse{};
    FixedE9 inside{}; // share of quotes the chosen model prices inside bid/ask
};

struct VenueResidual {
    VenueId venue{VenueId::Invalid};
    std::uint32_t quotes{0};
    FixedE9 mean{}; // mid iv minus the surface; positive is rich
    FixedE9 rms{};
};

struct Surface {
    UnixNanos as_of{};
    SurfaceModel model{SurfaceModel::None};
    SurfaceSource source{SurfaceSource::None};
    FixedE9 confidence{};
    SsviParams ssvi{};
    bool calendar_repair{false};    // svi slices crossed, so each takes the running max over earlier ones
    std::vector<SliceFit> slices{}; // nearest first
    FixedE9 rmse{};                 // the chosen model's
    FixedE9 svi_rmse{};
    FixedE9 ssvi_rmse{};
    std::vector<VenueResidual> venues{};
};

// per-expiry svi and one ssvi over the slices that fit; ssvi unless it trails svi by more than the tolerance
[[nodiscard]] Surface fit_surface(std::span<const SliceInput> slices, UnixNanos now, const SurfaceConfig& config = {});

struct SurfacePoint {
    FixedE9 vol{};
    FixedE9 skew{};         // ∂σ/∂ln K, what SmileAt carries into digital()
    FixedE9 dvol_dstrike{}; // ∂σ/∂K
    bool extrapolated{false};
};

// linear in total variance between expiries at fixed k; past the fitted strikes or the last expiry w is held flat,
// before the first σ is
[[nodiscard]] std::optional<SurfacePoint> surface_at(const Surface& surface, FixedE9 strike, FixedE9 years) noexcept;
[[nodiscard]] FixedE9 forward_at(const Surface& surface, FixedE9 years) noexcept;

struct ArbitrageReport {
    FixedE9 min_density{};
    std::uint32_t butterfly{0}; // grid points with g(k) below -1e-6
    std::uint32_t calendar{0};  // grid points where w falls from one expiry to the next
    bool ssvi_conditions{false};
};

[[nodiscard]] ArbitrageReport check_arbitrage(const Surface& surface) noexcept;

// the risk-neutral density factor g(k); a negative value is a butterfly arbitrage
[[nodiscard]] FixedE9 svi_density(const SviParams& svi, FixedE9 k) noexcept;
[[nodiscard]] FixedE9 ssvi_density(const SsviParams& ssvi, FixedE9 theta, FixedE9 k) noexcept;

// gatheral and jacquier, theorems 4.1 and 4.2, over every θ between the first and the last, which must not fall
[[nodiscard]] bool ssvi_arbitrage_free(const SsviParams& ssvi, std::span<const FixedE9> thetas) noexcept;

struct ProxyInput {
    UnixNanos as_of{};
    FixedE9 spot{};
    FixedE9 realized_vol{};           // the coin's, annualised
    FixedE9 reference_realized_vol{}; // the reference's over the same window
    std::span<const SliceInput> listed{};
};

// the reference's ssvi shape in k/√θ at the coin's level: θ fitted to its listed options where they fit,
// else the reference's θ scaled by the realized-vol ratio
[[nodiscard]] Surface proxy_surface(const Surface& reference, const ProxyInput& in, const SurfaceConfig& config = {});

// close-to-close, annualised over act/365
[[nodiscard]] FixedE9 realized_vol(std::span<const FixedE9> closes, std::uint64_t step_ns) noexcept;

} // namespace raikiri::options
