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
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitmex/wire.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace raikiri::venues::bitmex {

// Anything not exactly the fixed-width form yields the fallback, which callers
// set to ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos iso8601_to_unix(std::string_view text, UnixNanos fallback) noexcept;

// Exact numeric token -> 1e9 fixed point, reading both spellings BitMEX's two
// serialisers use for one field ("0.0001" and "1.0E-4"). Exposed because the
// book converts its own levels and must do it identically.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text);

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
    [[nodiscard]] std::optional<FundingUpdate>
    apply(const WireInstrumentTick& wire, const InstrumentBase& base, UnixNanos ts_init);

private:
    FixedE9 rate_{FixedE9::undef()};
    FixedE9 predicted_{FixedE9::undef()};
    UnixNanos next_funding_{};
    std::uint32_t interval_s_{0};
};

// The 24-hour turnover off the same `instrument` row. Separate from
// InstrumentState because it needs no merge: the two notionals move together
// and a frame restating neither must publish neither. BitMEX's own volume24h
// is in CONTRACTS and is deliberately not read — it compares with no other
// venue, and on an inverse row not even with itself in the base asset.
[[nodiscard]] std::optional<TickerStats> map_ticker_stats(const WireInstrumentTick& wire,
                                                          const exec::InstrumentTable& table,
                                                          UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// nullopt for anything not Open, not the requested market, quanto, or whose
// steps failed to convert exactly.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

// Execution ---------------------------------------------------------------

// What an `ordStatus` does to the order it names. Named separately from the
// event because an `update` frame's ordStatus is often the ONLY thing in it,
// and because a fill is reported by the `execution` table — which carries the
// price, the size and the exec id an order row does not.
enum class OrderEffect : std::uint8_t {
    Accepted,
    Canceled,
    Rejected,
    Expired,
    // Either the execution table already accounts for it, or it is a state
    // BitMEX passes through on the way somewhere. Silence beats inventing a
    // lifecycle event the OMS cannot undo.
    Silent,
};

[[nodiscard]] OrderEffect effect_of(std::string_view ord_status) noexcept;

// The columns one `order` row actually carried, converted against the
// instrument. EVERY member is optional: BitMEX keys the table on orderID alone
// and an `update` carries only what changed, so a caller merges this onto what
// it remembered rather than reading an absent column as zero.
struct OrderColumns {
    std::optional<Side> side;
    std::optional<OrderType> order_type;
    std::optional<OrderFlags> flags;
    std::optional<PriceTicks> price;
    std::optional<QtyLots> qty;
    std::optional<QtyLots> leaves;
    UnixNanos ts_event{};
};

// nullopt only when a column that IS present fails to convert exactly — a
// schema event, never something to round past.
[[nodiscard]] std::optional<OrderColumns>
map_order_columns(const WireOrder& wire, const InstrumentBase& base, UnixNanos ts_init);

// A REST /order row, which unlike a stream update always carries every column.
// nullopt for a status that is not working: the open-orders filter should never
// return one, and reporting a finished order as live is worse than dropping it.
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

// Only `execType: "Trade"` is a fill. Funding and Settlement rows carry a price
// and a quantity too, and booking them as trades invents a position that was
// never traded.
[[nodiscard]] std::optional<OrderEvent>
map_execution(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireExecution& wire,
                                                       const exec::InstrumentTable& table);

// `currentQty` is signed and counted in the venue's own units, so the sign
// carries the side and no side field can disagree with it. `avgEntryPrice` is
// an average of fills that no tick size need divide: the SIZE must convert
// exactly, and an average that does not is reported as no average rather than
// dropping a row and telling a reconciler we are flat.
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

} // namespace raikiri::venues::bitmex
