#pragma once

// fees and maintenance-margin tiers of one instrument on a venue account; the request and its answer cross shm

#include <raikiri/common/time.hpp>
#include <raikiri/domain/error_class.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace raikiri {

enum class FeeSource : std::uint8_t { Unknown = 0, Account, Schedule };

[[nodiscard]] constexpr std::string_view to_string(FeeSource source) noexcept {
    switch (source) {
    case FeeSource::Account:
        return "account";
    case FeeSource::Schedule:
        return "schedule";
    case FeeSource::Unknown:
        break;
    }
    return {};
}

enum class TiersSource : std::uint8_t { Default = 0, Venue };

[[nodiscard]] constexpr std::string_view to_string(TiersSource source) noexcept {
    return source == TiersSource::Venue ? "venue" : "default";
}

// what floor and cap count: quote notional, or base units where the venue brackets positions by size
enum class TierBasis : std::uint8_t { Notional = 0, BaseQty };

// maintenance margin inside the tier is notional * rate - amount; a zero cap or max_leverage was not stated
struct MaintenanceTier {
    double floor{0};
    double cap{0};
    double rate{0};
    double amount{0};
    double max_leverage{0};

    friend constexpr bool operator==(const MaintenanceTier&, const MaintenanceTier&) noexcept = default;
};

static_assert(sizeof(MaintenanceTier) == 40);

// the tier a notional falls in: the last whose floor it reaches, the first below every floor; null for no tiers
[[nodiscard]] constexpr const MaintenanceTier* tier_for(std::span<const MaintenanceTier> tiers,
                                                        double notional) noexcept {
    const MaintenanceTier* found = nullptr;
    for (const auto& tier : tiers) {
        if (found != nullptr && notional < tier.floor) {
            break;
        }
        found = &tier;
    }
    return found;
}

// tiers ascending by floor; the sign of `notional` is ignored
[[nodiscard]] constexpr std::optional<double> maintenance_margin(std::span<const MaintenanceTier> tiers,
                                                                 double notional) noexcept {
    const double size = notional < 0 ? -notional : notional;
    const auto* tier = tier_for(tiers, size);
    if (tier == nullptr) {
        return std::nullopt;
    }
    const double margin = size * tier->rate - tier->amount;
    return margin > 0 ? margin : 0.0;
}

// for a venue that states rates alone: the amounts that keep the margin continuous across each floor
constexpr void fill_maintenance_amounts(std::span<MaintenanceTier> tiers) noexcept {
    for (std::size_t i = 0; i < tiers.size(); ++i) {
        tiers[i].amount = i == 0 ? 0.0 : tiers[i - 1].amount + tiers[i].floor * (tiers[i].rate - tiers[i - 1].rate);
    }
}

// fees are fractions of notional, negative a rebate; Unknown leaves them to the venue's public schedule
struct TradingTerms {
    double maker_fee{0};
    double taker_fee{0};
    FeeSource fee_source{FeeSource::Unknown};
    TiersSource tiers_source{TiersSource::Default};
    TierBasis basis{TierBasis::Notional};
    std::vector<MaintenanceTier> tiers;
};

inline constexpr std::uint64_t k_terms_request_ttl_ns = 5'000'000'000ULL;

struct TermsRequest {
    std::uint64_t id{0};
    InstrumentId instrument{};
    UnixNanos ts_created{};
};

static_assert(sizeof(TermsRequest) == 24);
static_assert(std::is_trivially_copyable_v<TermsRequest>);

inline constexpr std::size_t k_terms_tiers_per_part = 8;
// a longer table is cut here, keeping its lowest tiers; every part of one answer must fit the reply ring
inline constexpr std::size_t k_terms_max_tiers = 160;

enum class TermsOutcome : std::uint8_t {
    Done = 0,
    Unsupported, // neither the venue nor the schedule states this instrument's fees
    Failed,      // the call did not complete; `cls` says how
};

// one answer is `parts` records with the same id, published in order; the head fields repeat on each
struct TermsReply {
    std::uint64_t id{0};
    InstrumentId instrument{};
    double maker_fee{0};
    double taker_fee{0};
    std::int64_t venue_code{0};
    TermsOutcome outcome{TermsOutcome::Done};
    ErrorClass cls{};
    FeeSource fee_source{FeeSource::Unknown};
    TiersSource tiers_source{TiersSource::Default};
    TierBasis basis{TierBasis::Notional};
    std::uint8_t part{0};
    std::uint8_t parts{1};
    std::uint8_t count{0}; // tiers in this part
    MaintenanceTier tiers[k_terms_tiers_per_part]{};
    char message[72]{}; // nul-terminated

    [[nodiscard]] std::string_view reason() const noexcept { return std::string_view{message}; }
};

static_assert(sizeof(TermsReply) == 440);
static_assert(std::is_trivially_copyable_v<TermsReply>);

} // namespace raikiri
