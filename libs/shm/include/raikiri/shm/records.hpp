#pragma once

// Wire records and the canonical gateway-segment layout. Every process maps
// these exact PODs; the segment's per-channel byte-size check (segment.hpp)
// is what turns accidental struct drift into a refused attach instead of
// silent corruption.
//
// Channel classes (fixed policy per channel, decided at design time):
//   catalogue    CatalogueTable write-once before publish; the id agreement
//   bbo/funding  SeqlockTable   state: overwrite-newest, stale ticks worthless
//   depth        SeqlockTable   state: the adapter's current book, not deltas
//   md_events    BroadcastRing  stream: lossy-with-gap-detect -> resync
//   exec_events  BroadcastRing  critical: overrun => halt venue + reconcile
//   cmd_*        SpscRing       command: lossless; Full = venue down

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/shm/broadcast_ring.hpp>
#include <raikiri/shm/catalogue.hpp>
#include <raikiri/shm/segment.hpp>
#include <raikiri/shm/seqlock_table.hpp>
#include <raikiri/shm/spsc_ring.hpp>

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

// Execution events carry the domain OrderEvent verbatim (it is exactly one
// ring slot already; the wrapper leaves room to diverge later without
// touching domain code).
struct ExecEventRec {
    OrderEvent event{};
};

static_assert(sizeof(ExecEventRec) == 192);
static_assert(std::is_trivially_copyable_v<ExecEventRec>);

// The command slot is the domain OrderCmd verbatim (defined in
// raikiri/domain/order_cmd.hpp — the shm layer transports vocabulary, it does
// not define it). Slot-property asserts live here where the ring needs them.
static_assert(sizeof(OrderCmd) == 128);
static_assert(std::is_trivially_copyable_v<OrderCmd>);

// ------------------------------------------------------------ layout ----

inline constexpr std::size_t k_bbo_cells = 4096;
inline constexpr std::size_t k_funding_cells = 4096;
inline constexpr std::size_t k_depth_cells = 4096;
inline constexpr std::size_t k_md_ring = 65536;
inline constexpr std::size_t k_exec_ring = 65536;
inline constexpr std::size_t k_cmd_ring = 4096;
inline constexpr std::size_t k_gateway_segment_bytes = 32ULL << 20;

// A SymbolId indexes both tables, so a catalogue that could hold more
// instruments than the BBO table has cells would mint ids with nowhere to
// quote from.
static_assert(k_catalogue_cells == k_bbo_cells);
static_assert(k_depth_cells == k_bbo_cells);

using BboTable = SeqlockTable<Bbo, k_bbo_cells>;
using FundingTable = SeqlockTable<FundingUpdate, k_funding_cells>;
using DepthTable = SeqlockTable<BookSnapshot, k_depth_cells>;
using MdRing = BroadcastRing<MdEvent, k_md_ring>;
using ExecRing = BroadcastRing<ExecEventRec, k_exec_ring>;
using CmdRing = SpscRing<OrderCmd, k_cmd_ring>;

// Resolved channel pointers of one gateway segment. Owner and client views
// are structurally identical; ownership semantics differ (segment.hpp).
struct GatewayChannels {
    CatalogueTable* catalogue{nullptr};
    BboTable* bbo{nullptr};
    DepthTable* depth{nullptr};
    FundingTable* funding{nullptr};
    MdRing* md_events{nullptr};
    ExecRing* exec_events{nullptr};
    CmdRing* cmd_create{nullptr};
    CmdRing* cmd_cancel{nullptr};
};

[[nodiscard]] std::expected<GatewayChannels, SegError> create_gateway_channels(SegmentOwner& owner);
[[nodiscard]] std::expected<GatewayChannels, SegError> open_gateway_channels(SegmentClient& client);

// <runtime_dir>/shm/gw.<venue> — one segment per gateway process.
[[nodiscard]] std::string gateway_segment_path(std::string_view venue_name);

} // namespace raikiri::shm
