#pragma once

#include <raikiri/domain/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::options {

// OptionGreeks holds iv as a fraction, vega per one vol point, theta per day and delta in the
// underlying's units per contract; greeks are counted in OptionInstrument::greeks
enum class IvScale : std::uint8_t { Fraction = 0, Percent };
enum class VegaScale : std::uint8_t { PerVolPoint = 0, PerUnitVol };
enum class ThetaScale : std::uint8_t { PerDay = 0, PerYear };

struct Convention {
    IvScale iv{IvScale::Fraction};
    VegaScale vega{VegaScale::PerVolPoint};
    ThetaScale theta{ThetaScale::PerDay};
};

// the venue's figures as parsed; nullopt leaves the field undefined, since a greek can be 0
struct VenueGreeks {
    std::optional<FixedE9> delta{};
    std::optional<FixedE9> gamma{};
    std::optional<FixedE9> vega{};
    std::optional<FixedE9> theta{};
    std::optional<FixedE9> rho{};
    std::optional<FixedE9> mark_iv{};
    std::optional<FixedE9> bid_iv{};
    std::optional<FixedE9> ask_iv{};
    std::optional<FixedE9> underlying_price{};
    std::optional<FixedE9> mark_price{};
    std::optional<FixedE9> index_price{};
};

[[nodiscard]] FixedE9 to_fraction(FixedE9 iv, IvScale scale) noexcept;
[[nodiscard]] FixedE9 to_per_vol_point(FixedE9 vega, VegaScale scale) noexcept;
[[nodiscard]] FixedE9 to_per_day(FixedE9 theta, ThetaScale scale) noexcept;

[[nodiscard]] OptionGreeks normalise(InstrumentId instrument,
                                     const VenueGreeks& venue,
                                     Convention convention,
                                     UnixNanos ts_event,
                                     UnixNanos ts_init) noexcept;

} // namespace raikiri::options
