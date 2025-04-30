#pragma once

// Wire types -> domain types.
//
// THE INVERSE DIRECTION. XBTUSD is quoted in USD, settled in BTC, and one
// contract is worth one USD, so contract_value holds a QUOTE-currency amount
// per contract on an inverse row and a BASE-currency amount on a linear one.
// map_instrument derives which from the row rather than assuming: backwards
// misprices every position and nothing downstream can catch it.
//
// TIMESTAMPS ARE TEXT — ISO-8601, never epoch numbers — so there is a
// hand-rolled parser below. None of it goes near parse_decimal_e9, which scales
// by 1e9 and would overflow int64 on an epoch value.

#include <raikiri/common/fixed_string.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitmex/wire.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace raikiri::venues::bitmex {

// Anything not exactly the fixed-width form yields the fallback, which callers
// set to ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos iso8601_to_unix(std::string_view text, UnixNanos fallback) noexcept;

// `fundingInterval` is a DURATION written as a timestamp:
// "2000-01-01T08:00:00.000Z" means eight hours. Zero when unreadable.
[[nodiscard]] std::uint32_t funding_interval_seconds(std::string_view text) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Top of book, merged across BitMEX's liquidity pools and across the one-sided
// frames the venue actually sends. The aggregate is the pools' sum at the best
// price — verified against the Aggregated orderBookL2_25 top, where Primary's
// 80200 plus Secondary's 2043500 is exactly the 2123700 reported at 64054.7.
// Letting the last frame win would flap between two books; reading Primary
// alone understated the top bid 26-fold on the same capture.
class QuoteBook {
public:
    // nullopt while a side is unseen (a fresh subscription starts one-sided) and
    // on any value that does not divide exactly by the instrument's step.
    [[nodiscard]] std::optional<Bbo>
    apply(const WireQuote& wire, const InstrumentBase& base, UnixNanos ts_init);

private:
    struct PoolTop {
        FixedString<16> pool{};
        PriceTicks bid{PriceTicks::undef()};
        PriceTicks ask{PriceTicks::undef()};
        QtyLots bid_qty{QtyLots::undef()};
        QtyLots ask_qty{QtyLots::undef()};
        bool used{false}; // an unnamed pool is legitimate, so occupancy is a flag
    };

    // Two pools exist today. A fifth is refused rather than folded into another
    // slot, which would double-count its size into the aggregate.
    std::array<PoolTop, 4> pools_{};
};

// Last-known funding columns of the `instrument` row, merged forward. The shm
// funding cell is overwritten whole, so a FundingUpdate assembled from an update
// frame carrying only markPrice would erase a real rate with a plausible zero.
// The `partial` primes this; a later update republishes the whole picture, and
// only when it moved a funding column — the mark-price updates that dominate
// this table are not funding events and must not read as one.
class InstrumentState {
public:
    // nullopt until a funding rate has been seen at all: a contract that quotes
    // no funding must never be reported as one funding at zero.
    [[nodiscard]] std::optional<FundingUpdate>
    apply(const WireInstrumentTick& wire, const InstrumentBase& base, UnixNanos ts_init);

private:
    FixedE9 rate_{FixedE9::undef()};
    FixedE9 predicted_{FixedE9::undef()};
    UnixNanos next_funding_{};
    std::uint32_t interval_s_{0};
};

// Catalogue ---------------------------------------------------------------

// nullopt for anything not Open, not the requested market, quanto, or whose
// steps failed to convert exactly — an instrument with an unusable tick size
// must never enter the table, because every later conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::bitmex
