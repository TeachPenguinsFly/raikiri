#pragma once

// quantities are base units e9 and prices quote per base unit e9, whatever the venue's lot or tick

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/origin.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace raikiri::joe {

enum class Algo : std::uint8_t { Sor, Twap, Vwap, Pov, Iceberg, Pegged, Stop, TakeProfit, TrailingStop };

enum class Profile : std::uint8_t { Flat, U, Recorded };

enum class Status : std::uint8_t { Pending, Working, Paused, Done, Cancelled, Failed };

enum class ChildTif : std::uint8_t { Ioc, Gtc, PostOnly };

enum class Refill : std::uint8_t { OnComplete, OnFill };

enum class PegTo : std::uint8_t { Primary, Mid, Market };

enum class TriggerOn : std::uint8_t { Mid, Last };

[[nodiscard]] std::string_view to_string(Algo algo) noexcept;
[[nodiscard]] std::string_view to_string(Profile profile) noexcept;
[[nodiscard]] std::string_view to_string(Status status) noexcept;
[[nodiscard]] std::string_view to_string(Refill refill) noexcept;
[[nodiscard]] std::string_view to_string(PegTo to) noexcept;
[[nodiscard]] std::string_view to_string(TriggerOn on) noexcept;

[[nodiscard]] constexpr bool resting(Algo algo) noexcept {
    return algo == Algo::Iceberg || algo == Algo::Pegged;
}

// waits for a price before it sends anything
[[nodiscard]] constexpr bool triggered(Algo algo) noexcept {
    return algo == Algo::Stop || algo == Algo::TakeProfit || algo == Algo::TrailingStop;
}

[[nodiscard]] constexpr bool terminal(Status status) noexcept {
    return status == Status::Done || status == Status::Cancelled || status == Status::Failed;
}

// one asset across venues: the catalogue's venue-neutral symbol and the market it trades on
struct Listing {
    SymbolText symbol{};
    MarketType market{MarketType::Perp};
};

struct IcebergSpec {
    FixedE9 show{FixedE9::undef()};
    // each show moves by up to this share of its size, either way
    std::uint32_t variance_bp{0};
    Refill refill{Refill::OnComplete};
};

// a positive offset sits further from the other side: a buy lower, a sell higher
struct PegSpec {
    PegTo to{PegTo::Primary};
    std::int32_t offset_ticks{0};
    std::int32_t offset_bp{0};
    std::uint32_t threshold_ticks{1};
    std::uint64_t min_interval_ns{1'000'000'000ULL};
};

// joe watches the price itself, so a trigger is the same on every venue and no venue holds it
struct TriggerSpec {
    // where a stop or a take-profit fires
    FixedE9 price{FixedE9::undef()};
    TriggerOn on{TriggerOn::Mid};
    // a trailing stop fires this far back from the best price since it became active
    std::uint32_t callback_bp{0};
    // a trailing stop is active once the price has moved this far in its favour; undef is at once
    FixedE9 activation{FixedE9::undef()};
};

// a bracket's exits, sent as reduce-only triggers once the entry has fills; an undef price is no such leg
struct AttachSpec {
    FixedE9 take_profit{FixedE9::undef()};
    FixedE9 take_profit_limit{FixedE9::undef()};
    FixedE9 stop_loss{FixedE9::undef()};
    FixedE9 stop_loss_limit{FixedE9::undef()};
    TriggerOn on{TriggerOn::Mid};

    [[nodiscard]] bool any() const noexcept { return !take_profit.is_undef() || !stop_loss.is_undef(); }
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
    // a child takes at most this share of its own venue's depth within depth_band_bp of that venue's touch, at least
    // what the venue will take; zero is no such limit
    std::uint32_t depth_share_bp{0};
    FixedE9 depth_band_bp{FixedE9::from_raw(10 * k_e9)};
    IcebergSpec iceberg{};
    PegSpec peg{};
    TriggerSpec trigger{};
    // where a resting algo sits; invalid takes the sor's best venue for each new child. a triggered algo reads its
    // price there and sends there
    VenueId venue{VenueId::Invalid};
    bool post_only{false};
    // the caller's own name for this order: a second start with a tag still held returns the first parent
    std::string client_tag{};
    // whose the children and their fills are; zero is the executor's own book
    std::uint16_t book{0};
    // an rfq trade id or a client's order id, on every child
    std::string external_ref{};
    // no child takes more than the book's position on its instrument closes; with none left the parent ends
    bool reduce_only{false};
    // parents that share one: the first to fire or fill cancels the others
    std::string oco{};
    AttachSpec attach{};
    // the entry a bracket leg closes; zero for any other parent
    std::uint64_t bracket_of{0};
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
    BadDepthShare,
    BadExpiry,
    NoRestLimit,
    NoShow,
    ShowTooBig,
    BadVariance,
    BothOffsets,
    BadOffset,
    NoThreshold,
    BadInterval,
    PostOnlyCrosses,
    TagTooLong,
    RefTooLong,
    NoTrigger,
    BadTrigger,
    NoCallback,
    BadActivation,
    BadTriggerExpiry,
    OcoTooLong,
    BadAttach,
};

[[nodiscard]] std::string_view to_string(SpecError error) noexcept;

[[nodiscard]] std::expected<void, SpecError> validate(const ParentSpec& spec) noexcept;

// a fired trigger with a limit rests there; one without takes what the books offer
[[nodiscard]] constexpr bool rests(const ParentSpec& spec) noexcept {
    return resting(spec.algo) || (triggered(spec.algo) && !spec.limit.is_undef());
}

// quote value of `qty` at `price`, both e9; saturates rather than wraps
[[nodiscard]] FixedE9 value_of(FixedE9 qty, FixedE9 price) noexcept;

// whole lots of `lot` in `qty`, rounded toward zero
[[nodiscard]] std::int64_t lots_in(FixedE9 qty, FixedE9 lot) noexcept;

[[nodiscard]] FixedE9 round_down(FixedE9 qty, FixedE9 lot) noexcept;

} // namespace raikiri::joe
