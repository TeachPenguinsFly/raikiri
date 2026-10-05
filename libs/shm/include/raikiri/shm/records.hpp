#pragma once

// every process maps these pods; each channel has exactly one writer

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/fill_report.hpp>
#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/domain/position_report.hpp>
#include <raikiri/shm/broadcast_ring.hpp>
#include <raikiri/shm/catalogue.hpp>
#include <raikiri/shm/segment.hpp>
#include <raikiri/shm/seqlock_table.hpp>
#include <raikiri/shm/spsc_ring.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <type_traits>

namespace raikiri::shm {

enum class MdKind : std::uint8_t { Trade = 0 };

// One market-data event, sized to exactly one cache line.
struct MdEvent {
    MdKind kind{MdKind::Trade};
    Side aggressor{Side::Buy};
    std::uint8_t pad_[6]{};
    InstrumentId instrument{};
    PriceTicks price{};
    QtyLots qty{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
    std::uint64_t venue_seq{0}; // gateway-local MD sequence, for gap forensics
    std::uint64_t reserved_{0};
};

static_assert(sizeof(MdEvent) == 64);
static_assert(std::is_trivially_copyable_v<MdEvent>);

struct ExecEventRec {
    OrderEvent event{};
};

static_assert(sizeof(ExecEventRec) == 192);
static_assert(std::is_trivially_copyable_v<ExecEventRec>);

// a ring so no funding charge overwrites another
struct LedgerEntryRec {
    LedgerEntry charge{};
};

static_assert(std::is_trivially_copyable_v<LedgerEntryRec>);

// an execution from the venue's trade history, written by the account poller alone
struct FillRec {
    FillReport fill{};
};

static_assert(std::is_trivially_copyable_v<FillRec>);

// fresh cells are the venue's open orders; a failed pull writes nothing so all cells age out
struct OpenOrderCell {
    ExchangeClientOrderId exchange_client_order_id{};
    InstrumentId instrument{};
    QtyLots leaves{};
    UnixNanos ts_venue{};
    UnixNanos ts_pulled{};
};

static_assert(std::is_trivially_copyable_v<OpenOrderCell>);

// a successful pull writes every instrument, zeros included; a failed one writes nothing so cells
// go stale
struct PositionCell {
    PositionReport report{};
    UnixNanos ts_pulled{};
};

static_assert(sizeof(PositionCell) == 40);
static_assert(std::is_trivially_copyable_v<PositionCell>);

// one cell so the whole balance set reads atomically; a failed pull writes nothing
inline constexpr std::size_t k_balance_rows = 64;

struct BalanceSnapshot {
    std::uint32_t count{0}; // rows in use; the rest are untouched, not zero
    UnixNanos ts_pulled{};
    std::array<BalanceReport, k_balance_rows> rows{};
};

static_assert(std::is_trivially_copyable_v<BalanceSnapshot>);

using BalanceTable = SeqlockTable<BalanceSnapshot, 1>;

static_assert(sizeof(OrderCmd) == 128);
static_assert(std::is_trivially_copyable_v<OrderCmd>);

inline constexpr std::size_t k_bbo_cells = 4096;
inline constexpr std::size_t k_funding_cells = 4096;
inline constexpr std::size_t k_greeks_cells = 4096;
inline constexpr std::size_t k_ticker_cells = 4096;
inline constexpr std::size_t k_depth_cells = 4096;
inline constexpr std::size_t k_position_cells = 4096;
inline constexpr std::size_t k_open_order_cells = 1024;
inline constexpr std::size_t k_md_ring = 65536;
inline constexpr std::size_t k_exec_ring = 65536;
inline constexpr std::size_t k_cmd_ring = 4096;
inline constexpr std::size_t k_funding_charge_ring = 4096;
// holds a busy account's executions over one pull window
inline constexpr std::size_t k_fill_ring = 8192;
// zeroed at creation, so this is resident memory per gateway
inline constexpr std::size_t k_gateway_segment_bytes = 64ULL << 20;

static_assert(k_catalogue_cells == k_bbo_cells);
static_assert(k_depth_cells == k_bbo_cells);
static_assert(k_position_cells == k_bbo_cells);
static_assert(k_greeks_cells == k_bbo_cells);
static_assert(k_ticker_cells == k_bbo_cells);

using BboTable = SeqlockTable<Bbo, k_bbo_cells>;
using FundingTable = SeqlockTable<FundingUpdate, k_funding_cells>;
using GreeksTable = SeqlockTable<OptionGreeks, k_greeks_cells>;
using TickerStatsTable = SeqlockTable<TickerStats, k_ticker_cells>;
using DepthTable = SeqlockTable<BookSnapshot, k_depth_cells>;
using PositionTable = SeqlockTable<PositionCell, k_position_cells>;
using OpenOrderTable = SeqlockTable<OpenOrderCell, k_open_order_cells>;
using MdRing = BroadcastRing<MdEvent, k_md_ring>;
using ExecRing = BroadcastRing<ExecEventRec, k_exec_ring>;
using LedgerEntryRing = BroadcastRing<LedgerEntryRec, k_funding_charge_ring>;
using FillRing = BroadcastRing<FillRec, k_fill_ring>;
using CmdRing = SpscRing<OrderCmd, k_cmd_ring>;
using MdReader = BroadcastReader<MdEvent, k_md_ring>;
using ExecReader = BroadcastReader<ExecEventRec, k_exec_ring>;
using LedgerEntryReader = BroadcastReader<LedgerEntryRec, k_funding_charge_ring>;
using FillReader = BroadcastReader<FillRec, k_fill_ring>;
using CmdProducer = SpscProducer<OrderCmd, k_cmd_ring>;
using CmdConsumer = SpscConsumer<OrderCmd, k_cmd_ring>;

// feed health over the gateway's rolling window; lag is ts_recv - ts_event on the local clock
struct ChannelMetrics {
    std::uint64_t messages{0};
    std::uint64_t window_ns{0};
    std::uint64_t lag_samples{0};
    std::int64_t lag_p50_ns{0};
    std::int64_t lag_p99_ns{0};
    std::int64_t lag_min_ns{0};
    std::uint64_t last_recv_ns{0};
};

enum class ClockSource : std::uint32_t { None = 0, RestProbe = 1 };

// which bounds the offset interval came from; Conflict when they do not meet
enum class OffsetSource : std::uint32_t { None = 0, Rest = 1, Feed = 2, RestAndFeed = 3, Conflict = 4 };

inline constexpr std::size_t k_metric_channels = 4; // bbo, trades, depth, funding

struct VenueMetrics {
    std::uint64_t published_ns{0};
    ClockSource clock_source{ClockSource::None};
    std::uint32_t probe_failures{0};
    // venue minus local
    std::int64_t offset_ns{0};
    std::uint64_t rtt_ns{0};
    std::uint64_t uncertainty_ns{0};
    std::uint64_t measured_ns{0};
    std::int64_t drift_ns{0};
    std::uint64_t drift_over_ns{0};
    OffsetSource offset_source{OffsetSource::None};
    std::uint32_t feed_bounded{0};
    // largest ts_event - ts_recv over the window, when feed_bounded
    std::int64_t feed_bound_ns{0};
    // int64 min and max stand for an open edge
    std::int64_t offset_lo_ns{0};
    std::int64_t offset_hi_ns{0};
    // what the gateway adds to the raw lag; zero means the raw lag stands
    std::int64_t correction_ns{0};
    std::uint64_t catalogue{0};
    std::uint64_t priced{0};
    std::uint64_t reopens{0};
    char endpoint[64]{};
    std::array<ChannelMetrics, k_metric_channels> channels{};
};

static_assert(std::is_trivially_copyable_v<VenueMetrics>);

using VenueMetricsCell = SeqlockCell<VenueMetrics>;

struct GatewayChannels {
    CatalogueTable* catalogue{nullptr};
    BboTable* bbo{nullptr};
    DepthTable* depth{nullptr};
    FundingTable* funding{nullptr};
    GreeksTable* greeks{nullptr};
    // latest-value 24-hour figures
    TickerStatsTable* ticker_stats{nullptr};
    MdRing* md_events{nullptr};
    ExecRing* exec_events{nullptr};
    // funding charges; must not lose one
    LedgerEntryRing* funding_charges{nullptr};
    // fills pulled from trade history, the only route on a venue without a private socket
    FillRing* fills{nullptr};
    PositionTable* positions{nullptr};
    OpenOrderTable* open_orders{nullptr};
    BalanceTable* balances{nullptr};
    CmdRing* cmd_create{nullptr};
    CmdRing* cmd_cancel{nullptr};
    // manual orders from admind; strategyd reads them so they meet the same risk gate
    CmdRing* order_intents{nullptr};
    VenueMetricsCell* metrics{nullptr};
};

[[nodiscard]] std::expected<GatewayChannels, SegError> create_gateway_channels(SegmentOwner& owner);
[[nodiscard]] std::expected<GatewayChannels, SegError> open_gateway_channels(SegmentClient& client);

// <runtime_dir>/shm/gw.<account>, one segment per gateway process
[[nodiscard]] std::string gateway_segment_path(AccountId account);

} // namespace raikiri::shm
