#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/extended/catalogue.hpp>
#include <raikiri/venues/extended/market_data.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::extended {

// No row or frame states the period.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// The wire says "-USD"; margin and settlement are in USDC.
inline constexpr std::string_view k_perp_quote = "USDC";

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

// Rate and mark arrive on two streams and the next settlement time on REST;
// the shm cell is written whole, so each publish is the merge of all three.
struct FundingMemo {
    FixedE9 rate{};
    PriceTicks mark{};
    UnixNanos next_funding{};
};

[[nodiscard]] bool note_rate(const WireFunding& wire, FundingMemo& memo) noexcept;

[[nodiscard]] bool note_mark(const WireMark& wire, const InstrumentBase& base, FundingMemo& memo) noexcept;

// The REST row can be a minute behind the streams, so it fills rate and mark
// only where they have not spoken yet.
void note_market(const WireMarket& wire, const InstrumentBase& base, FundingMemo& memo) noexcept;

// rate is the hour now accruing; a next settlement already past stays undef
[[nodiscard]] FundingUpdate
merged(const InstrumentBase& base, const FundingMemo& memo, UnixNanos ts_event, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireMarket& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::extended
