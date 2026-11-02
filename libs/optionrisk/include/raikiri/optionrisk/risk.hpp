#pragma once

// usd means the quote currency. A holding's `units` are contracts × contract_value, in the underlying,
// signed: + long. Greeks are black-76 on the forward at the vol that reprices the mark, so one convention
// holds on every venue:
//   delta_coin          underlying units of usd value delta, ∂V/∂F
//   delta_adjusted_coin the same less a coin premium, Δ − C/F; equal to delta_coin on a usd-settled contract
//   delta_usd           delta_coin × F
//   gamma               change in delta_coin per 1 usd of F
//   gamma_usd_1pct      change in delta_usd per a 1% move in F
//   vega_usd            usd per one vol point
//   theta_usd           usd per calendar day

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/optiondata/board.hpp>
#include <raikiri/options/key.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace raikiri::optionrisk {

struct Holding {
    std::string account{};
    InstrumentId instrument{};
    SymbolText symbol{};
    options::OptionKey key{};
    // what settles the contract; ContractKind does not say, okx's coin options are Inverse and deribit's Linear
    OptionUnit premium{OptionUnit::Unknown};
    FixedE9 contracts{FixedE9::from_raw(0)}; // signed, in the venue's quantity
    // underlying units per contract on every option venue, whatever ContractKind's comment says of a perp
    FixedE9 contract_value{FixedE9::undef()};
    FixedE9 entry{FixedE9::undef()}; // the venue's average, in `premium` per underlying unit
    UnixNanos as_of{};
};

[[nodiscard]] FixedE9 units_of(const Holding& holding) noexcept;

[[nodiscard]] bool coin_settled(const Holding& holding) noexcept;

// a × b, undef on an undefined factor or overflow
[[nodiscard]] FixedE9 times(FixedE9 a, FixedE9 b) noexcept;

enum class MarkFrom : std::uint8_t { None = 0, Venue, Mid, Model };

struct MarketState {
    FixedE9 mark{FixedE9::undef()}; // in the premium unit, per underlying unit
    MarkFrom mark_from{MarkFrom::None};
    UnixNanos mark_ts{};
    FixedE9 iv{FixedE9::undef()}; // the venue's mark iv
    UnixNanos greeks_ts{};
    FixedE9 forward{FixedE9::undef()};
    FixedE9 rate{FixedE9::undef()}; // what one coin of premium turns into usd at
};

// `forward` and `rate` answer where the quote's own greeks do not
[[nodiscard]] MarketState market_of(const optiondata::OptionQuote& quote, FixedE9 forward, FixedE9 rate) noexcept;

enum Flag : std::uint16_t {
    k_mark_missing = 1U << 0U,
    k_mark_stale = 1U << 1U,
    k_mark_model = 1U << 2U, // no quoted mark; the model priced it at the venue's iv
    k_greeks_missing = 1U << 3U,
    k_greeks_stale = 1U << 4U,
    k_position_stale = 1U << 5U,
    k_entry_missing = 1U << 6U,
    k_rate_missing = 1U << 7U,
};

struct Staleness {
    std::uint64_t mark_ns{60'000'000'000ULL};
    std::uint64_t greeks_ns{60'000'000'000ULL};
    std::uint64_t position_ns{90'000'000'000ULL};
};

struct PositionRisk {
    Holding holding{};
    MarketState market{};
    FixedE9 units{FixedE9::undef()};
    FixedE9 years{FixedE9::undef()};
    FixedE9 days_to_expiry{FixedE9::undef()};
    FixedE9 vol{FixedE9::undef()};         // the vol every model figure uses
    FixedE9 model_price{FixedE9::undef()}; // in the premium unit, at `vol`

    FixedE9 delta_coin{FixedE9::undef()};
    FixedE9 delta_adjusted_coin{FixedE9::undef()};
    FixedE9 delta_usd{FixedE9::undef()};
    FixedE9 gamma{FixedE9::undef()};
    FixedE9 gamma_usd_1pct{FixedE9::undef()};
    FixedE9 vega_usd{FixedE9::undef()};
    FixedE9 theta_usd{FixedE9::undef()};

    // unsuffixed money is in the premium unit; premium is + paid, − received, and a coin figure turns
    // into usd at today's rate, so value_usd − premium_usd = unrealized_usd holds
    FixedE9 value{FixedE9::undef()};
    FixedE9 value_usd{FixedE9::undef()};
    FixedE9 premium{FixedE9::undef()};
    FixedE9 premium_usd{FixedE9::undef()};
    FixedE9 unrealized{FixedE9::undef()};
    FixedE9 unrealized_usd{FixedE9::undef()};
    FixedE9 realized{FixedE9::undef()};
    FixedE9 realized_usd{FixedE9::undef()};

    std::uint16_t flags{0};
};

// `realized` is what the holding's fills booked, undef when unknown
[[nodiscard]] PositionRisk
assess(const Holding& holding, const MarketState& market, FixedE9 realized, UnixNanos now, const Staleness& staleness);

enum class SettledBy : std::uint8_t { Venue = 0, Index };

struct SettlementPrice {
    FixedE9 price{FixedE9::undef()};
    SettledBy by{SettledBy::Index};
    UnixNanos ts{};
};

// an index read this long past expiry is flagged late, since it is not the price the venue settled at
inline constexpr std::uint64_t k_settle_late_ns = 300'000'000'000ULL;

struct SettledPosition {
    Holding holding{};
    SettlementPrice at{};
    UnixNanos settled_at{};
    bool from_last_seen{false}; // the venue had already cleared the position when it was booked
    bool late{false};
    FixedE9 units{FixedE9::undef()};
    FixedE9 payoff{FixedE9::undef()};   // per underlying unit, in the premium unit
    FixedE9 proceeds{FixedE9::undef()}; // units × payoff, + received, − paid
    FixedE9 proceeds_usd{FixedE9::undef()};
    FixedE9 realized{FixedE9::undef()}; // units × (payoff − entry), gross of any delivery fee
    FixedE9 realized_usd{FixedE9::undef()};
};

// usd per underlying unit at `price`; 0 out of the money
[[nodiscard]] FixedE9 intrinsic(PutCall put_call, FixedE9 strike, FixedE9 price) noexcept;

[[nodiscard]] SettledPosition
settle(const Holding& holding, const SettlementPrice& at, UnixNanos settled_at, bool from_last_seen) noexcept;

struct Totals {
    FixedE9 delta_coin{FixedE9::from_raw(0)};
    FixedE9 delta_adjusted_coin{FixedE9::from_raw(0)};
    FixedE9 delta_usd{FixedE9::from_raw(0)};
    FixedE9 gamma{FixedE9::from_raw(0)};
    FixedE9 gamma_usd_1pct{FixedE9::from_raw(0)};
    FixedE9 vega_usd{FixedE9::from_raw(0)};
    FixedE9 theta_usd{FixedE9::from_raw(0)};
    FixedE9 premium_usd{FixedE9::from_raw(0)};
    FixedE9 value_usd{FixedE9::from_raw(0)};
    FixedE9 unrealized_usd{FixedE9::from_raw(0)};
    FixedE9 realized_usd{FixedE9::from_raw(0)};
    std::uint32_t positions{0};
    // positions whose figures could not all be summed; these totals understate the book when it is not 0
    std::uint32_t incomplete{0};
    std::uint32_t settled{0};
};

// what the fills of one contract booked, flat or not
struct BookedRealized {
    options::OptionKey key{};
    FixedE9 realized_usd{FixedE9::undef()};
};

struct UnderlyingRisk {
    UnderlyingText underlying{};
    Totals totals{};
};

struct ExpiryRisk {
    UnderlyingText underlying{};
    UnixNanos expiry{};
    FixedE9 days_to_expiry{FixedE9::undef()};
    Totals totals{};
};

struct Aggregates {
    std::vector<UnderlyingRisk> by_underlying{};
    std::vector<ExpiryRisk> by_expiry{}; // by underlying, nearest first
    Totals total{};                      // its coin figures mix underlyings, so they are not served
};

[[nodiscard]] Aggregates aggregate(std::span<const PositionRisk> positions,
                                   std::span<const SettledPosition> settled,
                                   std::span<const BookedRealized> booked,
                                   UnixNanos now);

struct ScenarioCell {
    FixedE9 spot{};
    FixedE9 vol{};
    FixedE9 value_usd{FixedE9::undef()};
    FixedE9 pnl_usd{FixedE9::undef()};
};

struct ScenarioMatrix {
    std::vector<FixedE9> spot{};       // as fractions: 0.1 is F × 1.1
    std::vector<FixedE9> vol{};        // as fractions: 0.05 is five vol points, across every expiry
    std::vector<ScenarioCell> cells{}; // row per vol, column per spot
    std::optional<std::size_t> worst{};
    FixedE9 base_value_usd{FixedE9::from_raw(0)};
    std::uint32_t positions{0};
    std::vector<InstrumentId> excluded{}; // no mark, vol or forward to revalue from
};

// a shocked vol floors here, so a large negative shock still prices
inline constexpr FixedE9 k_min_shocked_vol = FixedE9::from_raw(k_e9 / 100);

// the premium unit's value of one underlying unit under the shock, repriced in full; the mark's gap to the
// model at no shock carries through, so a zero shock returns the mark itself
[[nodiscard]] FixedE9 revalued(const PositionRisk& position, FixedE9 spot, FixedE9 vol) noexcept;

[[nodiscard]] ScenarioMatrix
scenario(std::span<const PositionRisk> positions, std::span<const FixedE9> spot, std::span<const FixedE9> vol);

} // namespace raikiri::optionrisk
