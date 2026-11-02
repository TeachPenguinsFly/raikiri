#pragma once

// what a book has sold: its net vega and gamma, which a periodic check holds against the book's limits since
// they cost a full reprice, and the row a desk reads

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/optionrisk/risk.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace raikiri::optionrisk {

// nullopt when the option has no key to value it by
[[nodiscard]] std::optional<Holding> holding_of(const OptionInstrument& option, QtyLots net, UnixNanos now) noexcept;

[[nodiscard]] MarketState market_of(const OptionGreeks& greeks) noexcept;

struct BookGreeks {
    FixedE9 vega_usd{FixedE9::from_raw(0)};
    FixedE9 gamma_usd_1pct{FixedE9::from_raw(0)};
    std::uint32_t incomplete{0}; // positions with no figure; a limit cannot be vouched for while this is not 0
};

[[nodiscard]] BookGreeks book_greeks(std::span<const PositionRisk> positions) noexcept;

// net short past a limit, or a limit set with a position it could not value; an undef limit is off
[[nodiscard]] bool past_short_limits(const BookGreeks& greeks, FixedE9 max_vega, FixedE9 max_gamma) noexcept;

struct ShortBookRow {
    std::uint16_t book_id{0};
    std::string book{};
    std::string venue{};
    std::string asset{}; // what the margin figures are in
    std::int64_t short_lots{0};
    std::int64_t working_lots{0};
    FixedE9 naked_usd{FixedE9::undef()};
    FixedE9 margin_held{FixedE9::undef()};
    FixedE9 margin_working{FixedE9::undef()};
    FixedE9 margin_available{FixedE9::undef()};
    FixedE9 asset_usd{FixedE9::undef()}; // usd per one of `asset`
    // the book's, across venues
    FixedE9 book_naked_usd{FixedE9::undef()};
    FixedE9 vega_usd{FixedE9::undef()};
    FixedE9 gamma_usd_1pct{FixedE9::undef()};
    FixedE9 max_naked_short{FixedE9::undef()};
    FixedE9 max_short_vega{FixedE9::undef()};
    FixedE9 max_short_gamma{FixedE9::undef()};
    bool reducing{false};
};

} // namespace raikiri::optionrisk
