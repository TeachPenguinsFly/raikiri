#pragma once

// one position in dollars; pure

#include <raikiri/bookrisk/ports.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/portfolio/position.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace raikiri::bookrisk {

struct Mark {
    // undef when the instrument has no two-sided quote
    PriceTicks mid{};
    // dollars per base unit from the venue, for a coin-premium option; undef elsewhere
    FixedE9 underlying{};
};

// adds one position to `into`
void add_position(PnlReading& into, const Instrument& listed, const portfolio::Position& position, Mark mark) noexcept;

// the kind of quote currency taken as a dollar
[[nodiscard]] bool is_dollar(std::string_view asset) noexcept;

// a set of balance rows in dollars; a unified wallet backs several markets, so one venue reports it more than once
class BalanceTally {
public:
    // `rate` is dollars per unit, undef when nothing prices the asset
    void add(VenueId venue, const BalanceReport& row, FixedE9 rate);

    void add_account() noexcept { ++reading_.accounts; }

    [[nodiscard]] const BalanceReading& reading() const noexcept { return reading_; }

private:
    struct Seen {
        VenueId venue{};
        AssetCode asset{};
    };

    std::vector<Seen> unified_{};
    BalanceReading reading_{};
};

} // namespace raikiri::bookrisk
