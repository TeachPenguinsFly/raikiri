#pragma once

// Wire records and the canonical gateway-segment layout. Every process maps
// these exact PODs; the segment's per-channel byte-size check (segment.hpp)
// is what turns accidental struct drift into a refused attach instead of
// silent corruption.
//
// Channel classes (fixed policy per channel, decided at design time):
//   catalogue    CatalogueTable write-once before publish; the id agreement
//   bbo/funding  SeqlockTable   state: overwrite-newest, stale ticks worthless
//   greeks       SeqlockTable   state: the venue's own option analytics
//   depth        SeqlockTable   state: the adapter's current book, not deltas
//   md_events    BroadcastRing  stream: lossy-with-gap-detect -> resync
//   exec_events  BroadcastRing  critical: overrun => halt venue + reconcile
//   positions    SeqlockTable   state: what the VENUE says it holds
//   funding_charges BroadcastRing ledger: the ACCOUNT POLLER writes it, alone
//   cmd_*        SpscRing       command: lossless; Full = venue down
//
// EVERY CHANNEL HAS ONE WRITER, and which one is part of the layout rather
// than a convention. funding_charges was the exception and was missing from
// this list, which is how it acquired a second: the gateway thread published
// what the private socket delivered while the poller thread published what it
// pulled, onto a ring that is single-producer by construction.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/domain/fill_report.hpp>
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

// Execution events carry the domain OrderEvent verbatim (it is exactly one
// ring slot already; the wrapper leaves room to diverge later without
// touching domain code).
struct ExecEventRec {
    OrderEvent event{};
};

static_assert(sizeof(ExecEventRec) == 192);
static_assert(std::is_trivially_copyable_v<ExecEventRec>);

// What the venue charged for carrying a position through a settlement.
//
// A RING, NOT A TABLE, and that is the whole point. A table answers "what is
// true now" and overwrites; funding is a sequence of separate charges, and one
// overwriting another is money that silently never happened. The seqlock
// tables above can afford to lose an update because the next one restates the
// whole truth — a charge restates nothing.
struct LedgerEntryRec {
    LedgerEntry charge{};
};

static_assert(std::is_trivially_copyable_v<LedgerEntryRec>);

// One execution the venue's TRADE HISTORY states, written by the account
// poller alone.
//
// NOT THE SAME FACT AS AN ExecEventRec, which is why it is not carried on that
// ring. An exec event comes off the private socket and states what the ORDER
// has left; this states the EXECUTION and nothing about the order. They also
// have different writers — the gateway thread owns exec_events and the poller
// owns this — and a BroadcastRing has exactly one producer.
struct FillRec {
    FillReport fill{};
};

static_assert(std::is_trivially_copyable_v<FillRec>);

// One order the VENUE says is still working, as of the pull that wrote it.
//
// FRESHNESS IS THE WHOLE SIGNAL, exactly as for positions below. A pass writes
// every order the venue reports and clears `ts_pulled` on the cells it did not
// use, so a fresh cell means "the venue still has this one" and an empty or
// stale one means the venue did not mention it. No count is written, because a
// count would have to be read atomically with the cells and cannot be.
//
// A pull that FAILED writes nothing at all, so every cell ages out together and
// a reader sees "the venue did not answer" rather than "the venue has no
// orders" — which is the difference between closing nothing and closing the
// whole book.
struct OpenOrderCell {
    ExchangeClientOrderId exchange_client_order_id{};
    InstrumentId instrument{};
    QtyLots leaves{};
    UnixNanos ts_venue{};
    UnixNanos ts_pulled{};
};

static_assert(std::is_trivially_copyable_v<OpenOrderCell>);

// What the venue says it holds, as of the pull that wrote it.
//
// ts_pulled is the whole design. A pull that succeeds writes EVERY catalogued
// instrument, zeros included, so a fresh cell of zero is the venue saying flat
// — a real answer. A pull that fails, or a venue with no position endpoint,
// writes NOTHING, so the cell goes stale and a reader that checks freshness
// reaches Unknown without anyone having to encode it. Absence of evidence
// stays absence of evidence instead of decaying into evidence of flatness.
struct PositionCell {
    PositionReport report{};
    UnixNanos ts_pulled{};
};

static_assert(sizeof(PositionCell) == 40);
static_assert(std::is_trivially_copyable_v<PositionCell>);

// WHAT THE ACCOUNT HOLDS, AS ONE CELL RATHER THAN A ROW PER ASSET.
//
// A balance set is a set: equity is the sum of it, and a reader that saw three
// assets of five would compute a number the account never had. One seqlock
// cell makes the whole set a single atomic read, so a reader either has the
// snapshot the venue sent or has the previous one — never a mixture of two.
//
// Same staleness rule as the positions above: a failed pull writes nothing and
// the cell ages out, so "the venue did not answer" never decays into "the
// account is empty".
inline constexpr std::size_t k_balance_rows = 64;

struct BalanceSnapshot {
    std::uint32_t count{0}; // rows in use; the rest are untouched, not zero
    UnixNanos ts_pulled{};
    std::array<BalanceReport, k_balance_rows> rows{};
};

static_assert(std::is_trivially_copyable_v<BalanceSnapshot>);

using BalanceTable = SeqlockTable<BalanceSnapshot, 1>;

// The command slot is the domain OrderCmd verbatim (defined in
// raikiri/domain/order_cmd.hpp — the shm layer transports vocabulary, it does
// not define it). Slot-property asserts live here where the ring needs them.
static_assert(sizeof(OrderCmd) == 128);
static_assert(std::is_trivially_copyable_v<OrderCmd>);

// ------------------------------------------------------------ layout ----

inline constexpr std::size_t k_bbo_cells = 4096;
inline constexpr std::size_t k_funding_cells = 4096;
inline constexpr std::size_t k_greeks_cells = 4096;
inline constexpr std::size_t k_ticker_cells = 4096;
inline constexpr std::size_t k_depth_cells = 4096;
inline constexpr std::size_t k_position_cells = 4096;
// A desk with more than this many orders resting on one venue at once has a
// problem no table size fixes.
inline constexpr std::size_t k_open_order_cells = 1024;
inline constexpr std::size_t k_md_ring = 65536;
inline constexpr std::size_t k_exec_ring = 65536;
inline constexpr std::size_t k_cmd_ring = 4096;
// Funding settles a handful of times a day per instrument, not per tick, so
// this is years of history rather than seconds of it.
inline constexpr std::size_t k_funding_charge_ring = 4096;
// A pull re-offers its whole window on every pass, and the window is days
// wide: this has to hold what a busy account executed over one, so that a
// reader attaching late still sees the pass rather than an overrun.
inline constexpr std::size_t k_fill_ring = 8192;
// 100 book levels a side put the depth table at 12.8 MiB, which no longer
// fits alongside the rings in 32. The whole table is zeroed at creation, so
// this is resident memory per gateway, not just address space.
inline constexpr std::size_t k_gateway_segment_bytes = 64ULL << 20;

// A SymbolId indexes both tables, so a catalogue that could hold more
// instruments than the BBO table has cells would mint ids with nowhere to
// quote from.
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

// Resolved channel pointers of one gateway segment. Owner and client views
// are structurally identical; ownership semantics differ (segment.hpp).
struct GatewayChannels {
    CatalogueTable* catalogue{nullptr};
    BboTable* bbo{nullptr};
    DepthTable* depth{nullptr};
    FundingTable* funding{nullptr};
    GreeksTable* greeks{nullptr};
    // Slow-moving beside the quote table: a 24-hour figure that a venue
    // restates on every ticker frame. Latest-value, so a reader that missed
    // an hour of frames still holds the right number.
    TickerStatsTable* ticker_stats{nullptr};
    MdRing* md_events{nullptr};
    ExecRing* exec_events{nullptr};
    // What the venue TOOK, beside what it quoted. The `funding` table above
    // holds the rate and is overwritten every tick; this carries the charges
    // and must not lose one.
    LedgerEntryRing* funding_charges{nullptr};
    // Executions pulled from the venue's trade history. The only route a fill
    // has on a venue with no private socket, and a net under the socket on
    // every other one — a fill the stream dropped is taken on the next pass.
    FillRing* fills{nullptr};
    PositionTable* positions{nullptr};
    // What the venue still has working. Read beside `positions`, because both
    // are the venue's word on this account and comparing numbers taken minutes
    // apart is how a reconciliation invents a break.
    OpenOrderTable* open_orders{nullptr};
    BalanceTable* balances{nullptr};
    CmdRing* cmd_create{nullptr};
    CmdRing* cmd_cancel{nullptr};
    // A HUMAN'S INTENT, on its way to the risk gate.
    //
    // admind writes here and strategyd reads; the command rings above stay
    // strategyd's alone. That indirection is not ceremony — these are SPSC
    // rings, so a second writer on cmd_create would be a data race, and the
    // losslessness the ring is chosen for depends on there being exactly one.
    // Routing the manual path through strategyd also means a hand-placed order
    // meets the same risk gate, the same halt and the same journal as one a
    // strategy sends, instead of walking around all three.
    //
    // It carries OrderCmd, the same 128-byte slot the command rings carry: an
    // intent and a command differ in who has vouched for them, not in shape.
    CmdRing* order_intents{nullptr};
};

[[nodiscard]] std::expected<GatewayChannels, SegError> create_gateway_channels(SegmentOwner& owner);
[[nodiscard]] std::expected<GatewayChannels, SegError> open_gateway_channels(SegmentClient& client);

// <runtime_dir>/shm/gw.<account> — one segment per gateway process, and one
// gateway process per market. The account name carries the market, so the
// perpetual gateway keeps the path it always had ("gw.bybit") and a spot one
// takes "gw.bybit-spot" instead of colliding with it.
[[nodiscard]] std::string gateway_segment_path(AccountId account);

} // namespace raikiri::shm
