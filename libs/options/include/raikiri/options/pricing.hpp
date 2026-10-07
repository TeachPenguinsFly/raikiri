#pragma once

// doubles stay inside pricing.cpp; everything here crosses as FixedE9 and comes back undef on bad input
// time is in years of 365 days, vega per vol point, theta per calendar day

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::options {

inline constexpr FixedE9 k_unit_e9 = FixedE9::from_raw(k_e9);

// act/365 to expiry; undef once expired
[[nodiscard]] FixedE9 years_between(UnixNanos now, UnixNanos expiry) noexcept;

struct Black76Input {
    FixedE9 forward{};
    FixedE9 strike{};
    FixedE9 vol{};
    FixedE9 years{};
    PutCall put_call{PutCall::Call};
    FixedE9 discount{k_unit_e9};
};

// greeks on the forward; theta holds the forward and carries the discount's own drift
struct Black76 {
    FixedE9 price{};
    FixedE9 delta{};
    FixedE9 gamma{};
    FixedE9 vega{};
    FixedE9 theta{};
};

[[nodiscard]] Black76 black76(const Black76Input& in) noexcept;

// coin-settled: premium in coin, delta = Δ − C/F, gamma = F·Γ − 2·delta, vega and theta divided by F
[[nodiscard]] Black76 inverse(const Black76Input& in) noexcept;

struct PremiumInput {
    FixedE9 price{};
    FixedE9 forward{};
    FixedE9 strike{};
    FixedE9 years{};
    PutCall put_call{PutCall::Call};
    // Base for a coin premium, which is priced against the forward
    OptionUnit unit{OptionUnit::Quote};
    FixedE9 discount{k_unit_e9};
};

// undef at or below intrinsic and at or above the forward
[[nodiscard]] FixedE9 implied_vol(const PremiumInput& in) noexcept;

struct SmilePoint {
    FixedE9 strike{};
    FixedE9 vol{};
};

struct SmileAt {
    FixedE9 vol{};
    // ∂σ/∂ln K
    FixedE9 skew{};
};

// linear in ln K between the listed strikes around `strike`, central on a listed one; no extrapolation
[[nodiscard]] std::optional<SmileAt> smile_at(std::span<const SmilePoint> smile, FixedE9 strike) noexcept;

struct DigitalInput {
    FixedE9 forward{};
    FixedE9 strike{};
    FixedE9 years{};
    SmileAt smile{};
};

// undiscounted probability of finishing above the strike
struct Digital {
    FixedE9 flat{};   // N(d2)
    FixedE9 skewed{}; // N(d2) − φ(d2)·√T·∂σ/∂ln K, i.e. N(d2) − Vega·∂σ/∂K
};

[[nodiscard]] Digital digital(const DigitalInput& in) noexcept;

struct TouchInput {
    FixedE9 forward{};
    FixedE9 barrier{};
    FixedE9 years{};
    SmileAt smile{}; // at the barrier
};

// undiscounted probability that a driftless forward touches the barrier before expiry, from either side
struct Touch {
    FixedE9 lognormal{};     // N(d2) + (F/H)·N(d1) for a barrier above, mirrored below
    FixedE9 twice_digital{}; // twice the skewed digital at the barrier
};

[[nodiscard]] Touch one_touch(const TouchInput& in) noexcept;

struct ParityQuote {
    FixedE9 strike{};
    FixedE9 call{};
    FixedE9 put{};
};

// median of K + (C − P)/df over the `strikes` pairs with the smallest |C − P|; for a coin premium
// C − P = (F − K)/F, so each pair gives K/(1 − (C − P)) instead
[[nodiscard]] std::optional<FixedE9> forward_from_parity(std::span<const ParityQuote> quotes,
                                                         OptionUnit unit,
                                                         FixedE9 discount = k_unit_e9,
                                                         std::size_t strikes = 5) noexcept;

enum class ForwardSource : std::uint8_t { None = 0, Parity, Future, Perp };

struct ForwardSources {
    std::optional<FixedE9> parity{};
    std::optional<FixedE9> future{}; // the same expiry's future mark
    std::optional<FixedE9> perp{};
    FixedE9 carry{FixedE9::from_raw(0)}; // the perp's carry to expiry, as a fraction
};

struct Forward {
    FixedE9 value{};
    ForwardSource source{ForwardSource::None};
};

// parity first, then the future, then perp × (1 + carry)
[[nodiscard]] Forward pick_forward(const ForwardSources& sources) noexcept;

} // namespace raikiri::options
