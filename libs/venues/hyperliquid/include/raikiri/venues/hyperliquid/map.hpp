#pragma once

// Wire types -> domain types.

#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/hyperliquid/account.hpp>
#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

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

// The 24-hour turnover off the same activeAssetCtx payload. No REST poll is
// involved: the channel that carries funding carries both volume columns.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Depth is a full snapshot on this venue, so the mapper needs no book state:
// the newest frame simply replaces what shm holds.
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBook& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// nullopt for a delisted asset and for any row whose derived steps are
// unusable.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireAsset& wire, MarketType market);

// A HIP-3 dex's row. The wire name already carries the dex ("xyz:TSLA") and
// stays the exchange symbol; the local symbol keeps the prefix too and takes the
// dex's collateral as its quote: "xyz:TSLA/USDC". Nothing on the wire says a
// ticker is an equity rather than a coin, and each dex prices its own oracle,
// so a bare "TSLA/USDC" would pair with whatever else shares the letters.
[[nodiscard]] std::optional<Instrument> map_dex_instrument(const WireAsset& wire,
                                                           std::string_view collateral);

// The class perpCategories states for a HIP-3 name. A name it omits takes the
// class the same ticker has on another dex ("io:EWY" from "xyz:EWY"); Unknown
// when neither says.
[[nodiscard]] AssetClass dex_asset_class(std::string_view name,
                                         std::span<const WireCategory> categories) noexcept;

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

// One settlement the venue charged, taken VERBATIM: `usdc` is already signed
// the way the account moved. nullopt for a row whose `type` is not funding, for
// a coin outside the table, and for a figure that will not read — an
// unreadable amount is not a zero charge.
//
// `reference` is BUILT, not carried. MEASURED over 500 consecutive rows: the
// `hash` beside every funding delta is 0x000…0, so it identifies nothing — and
// it is 66 characters against ExecId's 40 besides. The key is the coin and the
// settlement stamp instead: unique because funding settles once an hour per
// coin, and the same text on every replay, which is what stops a page the
// poller re-reads from double-booking.
[[nodiscard]] std::optional<LedgerEntry> map_funding_charge(const WireFundingCharge& wire,
                                                            const exec::InstrumentTable& table,
                                                            UnixNanos ts_init);

} // namespace raikiri::venues::hyperliquid
