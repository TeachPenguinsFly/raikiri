#pragma once

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

// hourly on every perp; scale before comparing with an 8h rate
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// zero or absent yields the fallback ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-decimals as e9 fixed point; nullopt when unrepresentable
[[nodiscard]] std::optional<FixedE9> step_from_decimals(std::int64_t decimals) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the context quotes no funding
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBook& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for a delisted asset and for any row whose derived steps are
// unusable.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireAsset& wire, MarketType market);

// the dex prefix stays in both symbols ("xyz:TSLA/USDC") since each dex prices its own oracle
[[nodiscard]] std::optional<Instrument> map_dex_instrument(const WireAsset& wire,
                                                           std::string_view collateral);

// a name perpCategories omits takes the class of the same ticker on another dex; unknown otherwise
[[nodiscard]] AssetClass dex_asset_class(std::string_view name,
                                         std::span<const WireCategory> categories) noexcept;

[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotAsset& wire);

// spot asset id is 10000 + pair index; perps use the row's position in meta.universe
inline constexpr std::int64_t k_spot_asset_offset = 10'000;

[[nodiscard]] std::int64_t spot_asset_id(std::int64_t pair_index) noexcept;

// "0x" + 32 lowercase hex.
inline constexpr std::size_t k_cloid_chars = 34;

// lossless transcode of the 128-bit ulid; empty when `id` is not a valid ulid
[[nodiscard]] std::string_view cloid_text(const ExchangeClientOrderId& id,
                                          std::span<char> out) noexcept;

// any 128-bit cloid decodes, so clients sharing a wallet cross-attribute fills
[[nodiscard]] bool cloid_to_ulid(std::string_view cloid, ExchangeClientOrderId& out) noexcept;

// refused unless the value divides the instrument's grid exactly
[[nodiscard]] std::optional<PriceTicks> price_from_wire(std::string_view text,
                                                        const InstrumentBase& base);
[[nodiscard]] std::optional<QtyLots> qty_from_wire(std::string_view text,
                                                   const InstrumentBase& base);

// `szi` is signed; nullopt when the coin is not in the table
[[nodiscard]] std::optional<exec::PositionReport>
map_position(const WirePosition& wire, const exec::InstrumentTable& table, UnixNanos ts_venue);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire,
                                                       const exec::InstrumentTable& table);

// `sz` is the remaining size and `origSz` the original
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table);

// nullopt for an unrecognised status word
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_order_status(const WireOrderStatus& wire, const exec::InstrumentTable& table);

// `usdc` is taken as signed; the reference is coin plus settlement stamp
// since the row hash is all zeroes
[[nodiscard]] std::optional<LedgerEntry> map_funding_charge(const WireFundingCharge& wire,
                                                            const exec::InstrumentTable& table,
                                                            UnixNanos ts_init);

} // namespace raikiri::venues::hyperliquid
