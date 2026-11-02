#pragma once

// a venue's published standard margin for a short option, as an estimate: per underlying unit, in
// OptionInstrument::premium units, so a coin option's figures are coin per coin

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::oms {

// Unpublished refuses every short opening: no figure, no order
enum class MarginFormula : std::uint8_t { Unpublished = 0, Deribit, Okx, Bybit, Gate };

struct ShortMarginRates {
    MarginFormula formula{MarginFormula::Unpublished};
    FixedE9 im_floor{FixedE9::from_raw(0)}; // the smaller initial rate
    FixedE9 im_base{FixedE9::from_raw(0)};  // the rate the out-of-the-money amount is taken from
    FixedE9 mm{FixedE9::from_raw(0)};
    FixedE9 liquidation_fee{FixedE9::from_raw(0)}; // of the index
    FixedE9 factor{FixedE9::from_raw(k_e9)};       // the seller's tier multiplier
};

[[nodiscard]] ShortMarginRates short_margin_rates(VenueId venue, std::string_view underlying) noexcept;

// index and forward are in quote; mark and order_price are in `premium`
struct ShortMarginInput {
    OptionUnit premium{OptionUnit::Unknown};
    PutCall put_call{PutCall::Call};
    FixedE9 strike{};
    FixedE9 index{};
    FixedE9 forward{};
    FixedE9 mark{};
    FixedE9 order_price{};
};

struct ShortMargin {
    FixedE9 initial{};
    FixedE9 maintenance{};
    FixedE9 order{}; // what opening at order_price reserves
};

enum class ShortMarginError : std::uint8_t { NoFormula, NoInput, WrongUnit };

[[nodiscard]] std::expected<ShortMargin, ShortMarginError> short_margin(const ShortMarginRates& rates,
                                                                        const ShortMarginInput& input) noexcept;

// underlying units of |lots|; undef with no lot or contract size
[[nodiscard]] FixedE9 option_units(const InstrumentBase& base, QtyLots lots) noexcept;

// usd a short pays at worst: underlying × units for a call, strike × units for a put, on coin and usd
// settled contracts alike; undef when the call has no underlying
[[nodiscard]] FixedE9 short_worst_case(const OptionInstrument& option, QtyLots lots, FixedE9 underlying) noexcept;

struct ShortLine {
    const OptionInstrument* option{nullptr};
    QtyLots position{QtyLots::from_raw(0)}; // signed
    QtyLots working_sell{QtyLots::from_raw(0)};
    FixedE9 index{};
    FixedE9 forward{};
    FixedE9 mark{};
};

// working sells count as filled and working buys do not offset them, as the lot limits do; one short leg of
// a spread counts in full, since a long at another strike is not netted
struct ShortFigures {
    QtyLots held{QtyLots::from_raw(0)};    // short now
    QtyLots working{QtyLots::from_raw(0)}; // what working sells would add
    FixedE9 naked_usd{FixedE9::from_raw(0)};
    FixedE9 margin_held{FixedE9::from_raw(0)};    // in the premium unit; undef when an input is missing
    FixedE9 margin_working{FixedE9::from_raw(0)}; // the same
};

[[nodiscard]] ShortFigures short_figures(const ShortLine& line, const ShortMarginRates& rates) noexcept;

// the asset a short's margin is held in: the underlying when coin settled, else the venue's stablecoin; empty
// where unknown, which finds no balance and so refuses the order
[[nodiscard]] std::string_view settlement_asset(const OptionInstrument& option) noexcept;

// the balance row backing an option settled in `asset`; undef when absent, unreported or older than max_age
[[nodiscard]] FixedE9 margin_available(std::span<const BalanceReport> rows,
                                       std::string_view asset,
                                       UnixNanos pulled,
                                       UnixNanos now,
                                       std::uint64_t max_age_ns) noexcept;

} // namespace raikiri::oms
