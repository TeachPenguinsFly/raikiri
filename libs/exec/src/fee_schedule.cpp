#include <raikiri/exec/terms.hpp>

// the base tier each venue publishes for an account with no volume, discount or referral, as fractions of notional.
// data only: a row is added or changed with its source and the day it was read, never from memory. a venue without
// a row answers no schedule fees, and a gateway holding a key asks the venue for the account's own rates instead

namespace raikiri::exec {

namespace {

struct Row {
    VenueId venue;
    MarketType market;
    BaseFees rates;
};

constexpr Row k_rows[] = {
    // binance usd-m futures, regular user, usdt pairs: maker 0.0200%, taker 0.0500%
    // https://www.binance.com/en/support/faq/360033544231 — read 2026-11-25
    {VenueId::Binance, MarketType::Perp, {.maker = 0.0002, .taker = 0.0005}},
    // bybit perpetual and futures, non-vip: maker 0.02%, taker 0.055%
    // https://www.bybit.com/en/help-center/article/Trading-Fee-Structure — read 2026-11-25
    {VenueId::Bybit, MarketType::Perp, {.maker = 0.0002, .taker = 0.00055}},
    // hyperliquid perps, tier 0: maker 0.015%, taker 0.045%
    // https://hyperliquid.gitbook.io/hyperliquid-docs/trading/fees — read 2026-11-25
    {VenueId::Hyperliquid, MarketType::Perp, {.maker = 0.00015, .taker = 0.00045}},
    // hyperliquid spot, tier 0: maker 0.040%, taker 0.070%; same page and day
    {VenueId::Hyperliquid, MarketType::Spot, {.maker = 0.0004, .taker = 0.0007}},
};

} // namespace

std::optional<BaseFees> base_fee_schedule(VenueId venue, MarketType market) noexcept {
    for (const auto& row : k_rows) {
        if (row.venue == venue && row.market == market) {
            return row.rates;
        }
    }
    return std::nullopt;
}

} // namespace raikiri::exec
