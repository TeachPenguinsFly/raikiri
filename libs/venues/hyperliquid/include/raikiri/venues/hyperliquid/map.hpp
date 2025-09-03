#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/hyperliquid/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace raikiri::venues::hyperliquid {

// Hourly and uniform across every perp, hence a constant — and why `funding`
// must not be compared against a CEX's 8h rate unscaled.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// Venue ms -> UnixNanos; zero/absent yields ingest time, because a zero
// propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-decimals as e9 fixed point, or nullopt when the exponent has no
// representation. Integer throughout: deriving a step through a double would
// reintroduce exactly the rounding the fixed-point domain exists to avoid.
[[nodiscard]] std::optional<FixedE9> step_from_decimals(std::int64_t decimals) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the context quotes no funding at all: a row without funding must
// not be reported as one funding at zero. Unlike okx, mark_price IS populated —
// it rides the same payload.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Depth is a full snapshot on this venue, so the mapper needs no book state:
// the newest frame simply replaces what shm holds.
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBook& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// nullopt for a delisted asset and for any row whose derived steps are
// unusable.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireAsset& wire, MarketType market);

// Spot rows come from a different response and a different index space, so
// they get their own mapper rather than a MarketType branch inside the one
// above: nothing about a perp row would be reusable here except the arithmetic.
[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotAsset& wire);

// The asset id an /exchange action names. Perps are the row's position in
// meta.universe; spot is 10000 + the PAIR index, a separate space in which
// asset 107 and asset 10107 are unrelated markets. Getting this wrong does not
// fail — it trades something else.
inline constexpr std::int64_t k_spot_asset_offset = 10'000;

[[nodiscard]] std::int64_t spot_asset_id(std::int64_t pair_index) noexcept;

// Client order ids -------------------------------------------------------

// "0x" + 32 lowercase hex.
inline constexpr std::size_t k_cloid_chars = 34;

// Our ULID rendered as this venue's `cloid`. A ULID is 48 timestamp bits plus
// 80 random ones — 128 bits EXACTLY, with the Crockford encoding's two spare
// bits always zero — so this is a total, reversible transcode of the same
// number into another base, not a hash and not a truncation. Empty when `id`
// is not a well-formed 26-char ULID.
[[nodiscard]] std::string_view cloid_text(const ExchangeClientOrderId& id,
                                          std::span<char> out) noexcept;

// The inverse, for attributing a fill to the order that caused it. Refuses
// anything that is not exactly "0x" + 32 hex.
//
// It cannot refuse a well-formed cloid minted by SOMEONE ELSE: unlike gate's
// `t-` prefix this field has no namespace to reserve, so any 128-bit id on the
// wallet decodes to a plausible ULID. Two clients sharing one wallet will
// cross-attribute, and there is no field on the wire that would let us tell.
[[nodiscard]] bool cloid_to_ulid(std::string_view cloid, ExchangeClientOrderId& out) noexcept;

// Wire decimal -> the instrument's own grid, EXACTLY. A value that does not
// divide evenly means the venue changed the instrument's precision, so it is
// refused rather than rounded onto a price the venue never quoted.
[[nodiscard]] std::optional<PriceTicks> price_from_wire(std::string_view text,
                                                        const InstrumentBase& base);
[[nodiscard]] std::optional<QtyLots> qty_from_wire(std::string_view text,
                                                   const InstrumentBase& base);

// Account reads ----------------------------------------------------------

// `szi` is signed and carries no side field to disagree with it. nullopt when
// the coin is not in the table — which, since the table holds only the markets
// this gateway loaded, is also how a perp row is skipped on a spot pull.
[[nodiscard]] std::optional<exec::PositionReport>
map_position(const WirePosition& wire, const exec::InstrumentTable& table, UnixNanos ts_venue);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire,
                                                       const exec::InstrumentTable& table);

// `sz` is the REMAINING size and `origSz` the original, so the two together
// are the only evidence of a partial fill this read carries.
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table);

// As above but with the venue's own status word applied on top. nullopt for a
// status this adapter does not recognise: answering Accepted for a word we
// cannot read would resurrect a dead order.
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_order_status(const WireOrderStatus& wire, const exec::InstrumentTable& table);

} // namespace raikiri::venues::hyperliquid
