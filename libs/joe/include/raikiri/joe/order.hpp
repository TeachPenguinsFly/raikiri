#pragma once

// quantities are base units e9 and prices quote per base unit e9, whatever the venue's lot or tick

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::joe {

enum class Algo : std::uint8_t { Sor, Twap, Vwap, Pov };

enum class Profile : std::uint8_t { Flat, U, Recorded };

enum class Status : std::uint8_t { Pending, Working, Paused, Done, Cancelled, Failed };

[[nodiscard]] std::string_view to_string(Algo algo) noexcept;
[[nodiscard]] std::string_view to_string(Profile profile) noexcept;
[[nodiscard]] std::string_view to_string(Status status) noexcept;

[[nodiscard]] constexpr bool terminal(Status status) noexcept {
    return status == Status::Done || status == Status::Cancelled || status == Status::Failed;
}

// one asset across venues: the catalogue's venue-neutral symbol and the market it trades on
struct Listing {
    SymbolText symbol{};
    MarketType market{MarketType::Perp};
};

struct ParentSpec {
    Listing listing{};
    Side side{Side::Buy};
    // one of the two; a notional becomes a quantity at the arrival mid
    FixedE9 qty{FixedE9::undef()};
    FixedE9 notional{FixedE9::undef()};
    FixedE9 limit{FixedE9::undef()};
    Algo algo{Algo::Twap};
    std::uint64_t horizon_ns{0};
    std::uint32_t slices{1};
    // each slice moves by up to this share of its interval, either way
    std::uint32_t jitter_bp{0};
    // a slice may run to this share of its planned size to catch up on a shortfall
    std::uint32_t catchup_pct{200};
    Profile profile{Profile::Flat};
    std::uint32_t pov_bp{0};
    FixedE9 max_child_notional{FixedE9::from_raw(20 * k_e9)};
};

enum class SpecError : std::uint8_t {
    NoSymbol,
    NoSize,
    BothSizes,
    BadLimit,
    NoSlices,
    NoHorizon,
    NoRate,
    NoChildCap,
};

[[nodiscard]] std::string_view to_string(SpecError error) noexcept;

[[nodiscard]] std::expected<void, SpecError> validate(const ParentSpec& spec) noexcept;

// quote value of `qty` at `price`, both e9; saturates rather than wraps
[[nodiscard]] FixedE9 value_of(FixedE9 qty, FixedE9 price) noexcept;

// whole lots of `lot` in `qty`, rounded toward zero
[[nodiscard]] std::int64_t lots_in(FixedE9 qty, FixedE9 lot) noexcept;

[[nodiscard]] FixedE9 round_down(FixedE9 qty, FixedE9 lot) noexcept;

} // namespace raikiri::joe
