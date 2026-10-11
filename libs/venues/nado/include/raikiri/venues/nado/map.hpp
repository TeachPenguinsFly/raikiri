#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/nado/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::nado {

// funding settles on the hour
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// an x18 integer at nine decimals; exact refuses a remainder below 1e-9, otherwise it is truncated
[[nodiscard]] std::optional<FixedE9> x18_to_e9(std::string_view text, bool exact) noexcept;

// perps only; a market that is not tradable is left out
[[nodiscard]] std::optional<Instrument> map_instrument(const WireSymbol& wire, MarketType market);

[[nodiscard]] std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

// the 24h rate the venue states, as the hourly rate it settles; mark and index from the contracts poll, ticks or undef
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, PriceTicks mark, PriceTicks index, UnixNanos ts_init);

// the contracts poll's decimals: mark and index floored onto the tick grid, and the 24h window
[[nodiscard]] std::optional<PriceTicks> map_price(std::string_view decimal, const InstrumentBase& base) noexcept;

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireContract& wire, const InstrumentBase& base, UnixNanos ts_init);

enum class BookResult : std::uint8_t {
    Resynced, // a snapshot replaced the book
    Applied,
    Stale,    // a push the snapshot already covers
    Ignored,  // no snapshot under it yet
    Gap,      // the chain broke: book dropped until the next snapshot
    BadLevel, // off the instrument's grid: book dropped until the next snapshot
};

// Chaining is by stamps: a push names the max_timestamp of the push before it.
// The first push after a snapshot must start at or before the snapshot's stamp.
class Book {
public:
    Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size);

    [[nodiscard]] BookResult resync(const WireDepth& snapshot);

    [[nodiscard]] BookResult apply(const WireDepth& push);

    void invalidate() noexcept;

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_init) const;

private:
    [[nodiscard]] bool apply_side(std::span<const WireLevel> levels, book::Ladder& side) const;

    InstrumentId instrument_{};
    FixedE9 tick_size_{};
    FixedE9 lot_size_{};
    book::Ladders ladders_;
    std::uint64_t snapshot_ts_{0};
    std::uint64_t last_max_ts_{0};
    bool synced_{false};
    bool chained_{false};
};

} // namespace raikiri::venues::nado
