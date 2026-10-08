#pragma once

// prices are usd decimals e9; ratios (spread, deviation, basis, dispersion) are e9 fractions

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::reference {

enum class QuoteCcy : std::uint8_t { Usd = 0, Usdt, Usdc, Other };

// the quote of "BTC/USDT", "xyz:TSLA/USDC" or "BTC/USD-PERP"
[[nodiscard]] QuoteCcy quote_ccy_of(std::string_view local_symbol) noexcept;
[[nodiscard]] std::string_view to_string(QuoteCcy ccy) noexcept;

// one book's top for one asset, in the book's quote currency; sizes are in the base asset
struct Quote {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Spot};
    QuoteCcy ccy{QuoteCcy::Other};
    FixedE9 bid{};
    FixedE9 ask{};
    FixedE9 bid_qty{};
    FixedE9 ask_qty{};
    // our receive time on the wall clock
    std::uint64_t ts_ns{0};
    FixedE6 volume_24h{};
};

enum class Cut : std::uint8_t {
    Empty = 0,
    Crossed,
    Wide,
    Stale,
    NoRate,
    Duplicate, // a second book of the same venue and market
    Warming,   // a perp whose basis to spot is not measured yet
    Outlier,
};

inline constexpr std::size_t k_cut_count = 8;

[[nodiscard]] std::string_view to_string(Cut cut) noexcept;

struct StableRates {
    FixedE9 usdt_usd{FixedE9::from_raw(k_e9)};
    FixedE9 usdc_usd{FixedE9::from_raw(k_e9)};
    bool usdt_fallback{true};
    bool usdc_fallback{true};
    std::uint16_t usdt_sources{0};
    std::uint16_t usdc_sources{0};
};

enum class Weighting : std::uint8_t {
    Volume,    // 24h quote turnover
    Liquidity, // the smaller side's touch notional
};

struct Params {
    std::int64_t max_spread_e9{5'000'000};
    std::uint64_t stale_spot_ns{10'000'000'000ULL};
    std::uint64_t stale_perp_ns{2'000'000'000ULL};
    // excluded beyond this deviation from the median, readmitted below half of it
    std::int64_t outlier_e9{20'000'000};
    std::size_t min_sources{3};
    Weighting weighting{Weighting::Volume};
    // a source weighs at most this many times the median weight
    std::int64_t weight_cap{3};
    std::int64_t perp_weight_pct{50};
    std::uint64_t basis_tau_ns{150'000'000'000ULL};
    std::uint64_t basis_warm_ns{30'000'000'000ULL};
    FixedE9 usdt_fallback{FixedE9::from_raw(k_e9)};
    FixedE9 usdc_fallback{FixedE9::from_raw(k_e9)};
};

// what one venue and market carries between cycles
struct SourceState {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Spot};
    bool excluded{false};
    // perp over spot minus one, smoothed
    std::int64_t basis_e9{0};
    std::uint64_t basis_since_ns{0};
    std::uint64_t basis_at_ns{0};
};

struct AssetState {
    std::vector<SourceState> sources{};
};

struct UsedSource {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Spot};
    QuoteCcy ccy{QuoteCcy::Other};
    FixedE9 price{}; // usd, basis-adjusted for a perp
    std::int64_t weight{0};
};

struct CutSource {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Spot};
    Cut cut{Cut::Empty};
};

struct Reference {
    // undef when refused
    FixedE9 mid{};
    FixedE9 spot_mid{};
    // the perps' own level, not basis-adjusted
    FixedE9 perp_mid{};
    // perp_mid over mid minus one
    FixedE9 basis_e9{};
    // half-width in usd: the larger of half the weighted interquartile range and the median half-spread
    FixedE9 conf{};
    // weighted median absolute deviation over the mid
    FixedE9 dispersion_e9{};
    std::uint16_t quotes{0};
    std::uint16_t used_spot{0};
    std::uint16_t used_perp{0};
    std::array<std::uint16_t, k_cut_count> cuts{};
    std::uint64_t oldest_age_ns{0};
    bool refused{true};
    StableRates rates{};
    std::vector<UsedSource> used{};
    std::vector<CutSource> cut{};

    [[nodiscard]] std::size_t used_count() const noexcept { return used.size(); }
};

// the usdt rate from usd books of usdt; the usdc rate from usd books of usdc and from usdc/usdt times the usdt rate
[[nodiscard]] StableRates stable_rates(std::span<const Quote> usdt_books,
                                       std::span<const Quote> usdc_books,
                                       const Params& params,
                                       std::uint64_t now_ns);

[[nodiscard]] Reference compute(std::span<const Quote> quotes,
                                const StableRates& rates,
                                const Params& params,
                                AssetState& state,
                                std::uint64_t now_ns);

namespace internal {

// a*b/d for venue figures: empty on a zero divisor or past i64 rather than an abort
[[nodiscard]] std::optional<std::int64_t> scaled(std::int64_t a, std::int64_t b, std::int64_t d) noexcept;

struct Weighted {
    std::int64_t price{0};
    std::int64_t weight{0};
};

// sorts in place; an exact half split averages the two neighbours
[[nodiscard]] std::int64_t weighted_quantile(std::vector<Weighted>& points, std::int64_t num, std::int64_t den);

[[nodiscard]] std::int64_t plain_median(std::vector<std::int64_t> values);

} // namespace internal

} // namespace raikiri::reference
