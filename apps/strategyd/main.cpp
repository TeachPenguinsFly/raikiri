// strategyd — the strategy kernel (demo wiring).
//
//   strategyd [--gateway=<account>]... [--duration <sec>] [--strategy-id=<n>]
//
// --gateway is repeatable and takes an account name, which is also the segment
// name: "bybit" is the perpetual gateway, "bybit-spot" the spot one beside it.
// --venue is the same flag under its older name. One kernel holds ONE book
// across every market it attaches to, because a position is a position
// wherever it was opened; the (venue, market) prefix of an instrument id is
// what routes a command back to the gateway that can fill it.
//
// Attaches each gateway's segment, adopts the instrument numbering it published,
// runs a strategy through the OMS and prints the latency table at exit — the
// `make demo` money-shot:
//   tick-to-callback   BBO publish (gatewayd) -> strategy callback
//   submit-call        ctx.submit() wall time (risk + store + journal + ring)
//   order-round-trip   submit -> first venue verdict event back
//
// Single thread, no locks: exec ring, md ring, BBO change-gates, timers, all
// polled in one loop — the same shape the real kernel keeps forever.

#include <raikiri/journal/detail.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/observ/histogram.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/order_store.hpp>
#include <raikiri/oms/risk.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/portfolio/reconcile.hpp>
#include <raikiri/portfolio/target.hpp>
#include <raikiri/schema/codec.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/strategies/demo_maker.hpp>
#include <raikiri/strategy/strategy.hpp>

#include <fmt/format.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

using msgs = raikiri_journal::schema::messages;
using namespace raikiri;

// Every Strategy callback defaults to a no-op, so observing a live venue
// needs no separate code path.
class Observer final : public strategy::Strategy {};

// One attached gateway — everything that is per-(venue, market).
//
// A gateway serves exactly one market, so anything scoped to a segment is
// scoped to an account: the catalogue that mints the instrument ids, the
// tables those ids index, the rings, and the change-gate cursors over them.
// The book, the order store and the risk gate are NOT here: they span venues
// on purpose, because a position is a position wherever it was opened.
struct Gateway {
    AccountId account{};
    std::string name;
    shm::SegmentClient client;
    shm::GatewayChannels ch{};
    shm::ExecReader exec;
    shm::LedgerEntryReader charges;
    shm::MdReader md;
    shm::CmdProducer creates;
    shm::CmdProducer cancels;
    shm::CmdConsumer intents;
    std::vector<std::uint32_t> bbo_seen;
    std::vector<std::uint32_t> funding_seen;
    std::uint64_t recon_due{0};

    Gateway(AccountId id,
            std::string display,
            shm::SegmentClient attached,
            shm::GatewayChannels channels)
        : account{id}, name{std::move(display)}, client{std::move(attached)}, ch{channels},
          exec{channels.exec_events}, charges{channels.funding_charges}, md{channels.md_events},
          creates{channels.cmd_create}, cancels{channels.cmd_cancel},
          intents{channels.order_intents},
          bbo_seen(std::min<std::size_t>(channels.catalogue->size(), shm::k_bbo_cells)),
          funding_seen(std::min<std::size_t>(channels.catalogue->size(), shm::k_funding_cells)) {}
};

class KernelContext final : public strategy::Context {
public:
    KernelContext(std::vector<Gateway>& gateways,
                  oms::OrderStore& store,
                  oms::RiskGate& risk,
                  oms::IdGen& ids,
                  journal::JournalWriter& journal,
                  const Clock& clock)
        : gateways_{&gateways}, store_{&store}, risk_{&risk}, ids_{&ids}, journal_{&journal},
          clock_{&clock} {}

    // The (venue, market) prefix of an InstrumentId IS the routing key — it is
    // why the id carries one. Linear over a handful of attached gateways; a
    // map would cost more than the scan it replaces.
    [[nodiscard]] Gateway* gateway_for(InstrumentId id) const noexcept {
        const AccountId want{.venue = id.venue(), .market = id.market()};
        for (auto& gw : *gateways_) {
            if (gw.account == want) {
                return &gw;
            }
        }
        return nullptr;
    }

    std::expected<ClientOrderId, strategy::RejectReason>
    submit(const strategy::NewOrder& order) noexcept override {
        return submit_as(order, ids_->next_client_id());
    }

    /** Place an order under an id somebody else already minted.
     *
     *  The manual desk needs this: admind answered its caller with a client
     *  order id before the intent ever reached this process, and the book has
     *  to agree with what that caller was told. Everything else — the risk
     *  gate, the store, the journal-before-wire, the local Submitted
     *  transition — is the one path both callers take, because a hand-placed
     *  order that skipped any of it would be an order the system cannot
     *  reconcile, halt or replay. */
    std::expected<ClientOrderId, strategy::RejectReason> submit_as(const strategy::NewOrder& order,
                                                                   ClientOrderId id) noexcept {
        const auto t0 = clock_->mono();

        OrderCmd cmd{};
        cmd.kind = CmdKind::Create;
        cmd.side = order.side;
        cmd.order_type = order.type;
        cmd.tif = order.tif;
        cmd.flags = order.flags;
        cmd.instrument = order.instrument;
        cmd.client_order_id = id;
        cmd.exchange_client_order_id = ids_->next_exchange_client_id(clock_->wall());
        cmd.price = order.price;
        cmd.qty = order.qty;
        cmd.ts_created = clock_->wall();

        auto* gw = gateway_for(order.instrument);
        if (gw == nullptr) {
            // No gateway is attached for this instrument's market. Refusing as
            // VenueDown is exact: the venue is not down, but from here it is
            // unreachable, and both mean the order never gets to the wire.
            return std::unexpected{strategy::RejectReason::VenueDown};
        }
        oms::Exposure exposure{.reconciliation_break = halted.contains(order.instrument.packed),
                               .open_orders = store_->open_count()};
        if (const auto* held = position(order.instrument); held != nullptr) {
            exposure.position_lots = held->qty();
        }
        const auto working = oms::working_lots(*store_, order.instrument);
        exposure.working_buy_lots = working.buy;
        exposure.working_sell_lots = working.sell;
        // The mid, where there is one. Left undefined otherwise, which turns
        // the band off rather than banding against a price nobody published.
        if (const auto quote = bbo(order.instrument);
            quote.has_value() && !quote->bid.is_undef() && !quote->ask.is_undef()) {
            exposure.reference_price = PriceTicks::from_raw((quote->bid.raw + quote->ask.raw) / 2);
        }
        // The gate knows exactly which rule refused; `RejectReason` has one code
        // for all ten. Named here because this is the last place the specific
        // reason exists — past this return it is gone.
        if (const auto allowed = risk_->check(cmd, exposure); !allowed.has_value()) {
            RLOG_WARN("strategyd: risk refused coid={} — {}",
                      cmd.client_order_id.raw,
                      oms::to_string(allowed.error()));
            return std::unexpected{strategy::RejectReason::Risk};
        }
        if (!store_->create(cmd, cmd.exchange_client_order_id).has_value()) {
            return std::unexpected{strategy::RejectReason::Capacity};
        }
        // Intent journaled BEFORE the wire: the crash-recovery invariant.
        journal_cmd(cmd);
        if (!gw->creates.try_push(cmd).has_value()) {
            return std::unexpected{strategy::RejectReason::VenueDown};
        }
        // The order is on the wire: apply the local Submitted transition.
        // Without this the FSM (correctly!) rejects the venue's Accepted —
        // Initialized has no Accepted arm.
        OrderEvent submitted{};
        submitted.type = OrderEventType::Submitted;
        submitted.side = cmd.side;
        submitted.order_type = cmd.order_type;
        submitted.tif = cmd.tif;
        submitted.flags = cmd.flags;
        submitted.instrument = cmd.instrument;
        submitted.client_order_id = cmd.client_order_id;
        submitted.exchange_client_order_id = cmd.exchange_client_order_id;
        submitted.qty = cmd.qty;
        submitted.leaves_qty = cmd.qty;
        submitted.price = cmd.price;
        submitted.ts_init = cmd.ts_created;
        (void)store_->apply_event(submitted);

        inflight_[cmd.client_order_id.raw] = t0.ns;
        submit_hist.record(clock_->mono().ns - t0.ns);
        return cmd.client_order_id;
    }

    bool cancel(ClientOrderId id) noexcept override {
        const auto* order = store_->find(id);
        if (order == nullptr) {
            return false;
        }
        OrderCmd cmd{};
        cmd.kind = CmdKind::Cancel;
        cmd.instrument = order->instrument();
        cmd.client_order_id = id;
        cmd.exchange_client_order_id = order->exchange_client_order_id();
        cmd.target_exchange_order_id = order->exchange_order_id();
        cmd.ts_created = clock_->wall();
        auto* gw = gateway_for(cmd.instrument);
        if (gw == nullptr) {
            return false;
        }
        journal_cmd(cmd);
        return gw->cancels.try_push(cmd).has_value();
    }

    void cancel_all(InstrumentId instrument) noexcept override {
        store_->for_each_open([&](const oms::Order& order) {
            if (order.instrument() == instrument) {
                (void)cancel(order.client_order_id());
            }
        });
    }

    std::optional<Bbo> bbo(InstrumentId instrument) const noexcept override {
        const auto idx = instrument.symbol().raw;
        const auto* gw = gateway_for(instrument);
        if (gw == nullptr || idx >= shm::k_bbo_cells) {
            return std::nullopt;
        }
        Bbo out{};
        if (gw->ch.bbo->read(idx, out) == 0) {
            return std::nullopt;
        }
        return out;
    }

    const Instrument* instrument(InstrumentId id) const noexcept override {
        const auto* gw = gateway_for(id);
        return gw != nullptr ? gw->ch.catalogue->find(id) : nullptr;
    }

    std::optional<ClientOrderId>
    set_target(const portfolio::PortfolioTarget& target) noexcept override {
        auto held = QtyLots::from_raw(0);
        if (const auto* p = position(target.instrument); p != nullptr) {
            held = p->qty();
        }
        const auto working = oms::working_lots(*store_, target.instrument);
        const auto move = portfolio::delta_to_target(target.qty, held, working.buy, working.sell);
        if (move.qty.raw == 0) {
            return std::nullopt;
        }
        // Market, because a target says WHAT to hold and not at what price. A
        // strategy that cares about the price is asking for a different thing
        // and should submit the order itself.
        const auto sent = submit({.instrument = target.instrument,
                                  .side = move.side,
                                  .type = OrderType::Market,
                                  .tif = TimeInForce::Ioc,
                                  .qty = move.qty});
        return sent.has_value() ? std::optional{*sent} : std::nullopt;
    }

    const portfolio::Position* position(InstrumentId id) const noexcept override {
        return positions.find({ids_->strategy_id(), id});
    }

    FixedE9 unrealized(InstrumentId id, PriceTicks mark) const noexcept override {
        const auto* held = position(id);
        const auto* listed = instrument(id);
        if (held == nullptr || listed == nullptr) {
            return FixedE9::from_raw(0);
        }
        return portfolio::unrealized_pnl(base_of(*listed), *held, mark);
    }

    void set_timer(std::uint32_t timer_id, std::uint64_t delay_ns) noexcept override {
        const auto deadline = clock_->mono().ns + delay_ns;
        for (auto& timer : timers) {
            if (timer.id == timer_id) {
                timer.deadline_ns = deadline; // re-arm replaces
                return;
            }
        }
        timers.push_back({timer_id, deadline});
    }

    const Clock& clock() const noexcept override { return *clock_; }

    // --- kernel-side state the main loop reads ---
    // A fee this process wrote, kept only long enough for the venue's ledger to
    // quote it back.
    struct FeeMemo {
        FixedE9 amount{};
        UnixNanos booked_at{};
    };

    struct Timer {
        std::uint32_t id{0};
        std::uint64_t deadline_ns{0};
    };

    std::vector<Timer> timers;
    portfolio::PositionBook positions;
    // Instruments this process has stopped opening on. Cleared only by a
    // restart, never by one agreeing pull: an automatic clear on a single
    // agreement re-opens the window the break was reporting.
    std::unordered_set<std::uint64_t> halted;
    // Last verdict per instrument, so a standing condition is reported once
    // rather than every read. A break that persists is not new information,
    // and a line repeated every five seconds trains an operator to skip it.
    std::unordered_map<std::uint64_t, portfolio::ReconVerdict> recon_seen;
    // Counted per comparison, not per instrument: a run that agrees prints
    // nothing, and "nothing" has to be distinguishable from "never ran".
    std::uint64_t recon_agreed{0};
    // Orders the venue had dropped while this process still held them open.
    std::uint64_t orders_closed_by_recon{0};

    // What this process booked as a fee, keyed by the venue's own id for the
    // execution that incurred it — which is what the venue's ledger quotes back
    // when it states the same fee. Bounded and pruned: a desk that never
    // reconciles must not grow a map for the life of the process.
    std::unordered_map<std::string, FeeMemo> booked_fees;
    std::uint64_t fees_agreed{0};
    std::uint64_t fees_disagreed{0};
    std::uint64_t fees_unmatched{0};

    // The venue's own record of a fee, against what this process wrote.
    //
    // NEVER BOOKED. The fill already wrote it; this is the check that the two
    // agree, and a disagreement is a fact for a human rather than a correction
    // to apply automatically — the venue's number and ours are both evidence,
    // and silently overwriting one with the other loses the disagreement.
    void reconcile_fee(const LedgerEntry& charge, const InstrumentBase& base) {
        const std::string key{charge.reference.view()};
        const auto it = booked_fees.find(key);
        if (it == booked_fees.end()) {
            ++fees_unmatched;
            // Either a fill this process never saw, or the venue quotes a
            // different id here than on the execution — both worth a line,
            // because one is lost money and the other is a wrong join.
            RLOG_WARN("strategyd: FEE {} on {} — venue charged {}.{:09} against no fill we hold",
                      key,
                      base.exchange_symbol.view(),
                      charge.amount.raw / 1'000'000'000,
                      std::abs(charge.amount.raw) % 1'000'000'000);
            return;
        }
        if (it->second.amount.raw == charge.amount.raw) {
            ++fees_agreed;
            return;
        }
        ++fees_disagreed;
        RLOG_ERROR("strategyd: FEE BREAK {} on {} — we booked {} e9, venue says {} e9",
                   key,
                   base.exchange_symbol.view(),
                   it->second.amount.raw,
                   charge.amount.raw);
    }

    // Dropped once older than the window a venue's ledger still reaches back
    // over: past that the venue will never quote the fee again, so keeping the
    // memo only grows the map.
    void prune_booked_fees(UnixNanos now, std::uint64_t max_age_ns) {
        std::erase_if(booked_fees, [&](const auto& entry) {
            return now.ns > entry.second.booked_at.ns &&
                   now.ns - entry.second.booked_at.ns > max_age_ns;
        });
    }

    // The order book, for the reconciliation that compares it against what the
    // venue still has working. Public for the same reason `positions` is: the
    // main loop owns the comparison, the context owns the state.
    [[nodiscard]] oms::OrderStore& orders() noexcept { return *store_; }

    std::uint64_t recon_unknown{0};
    std::uint64_t recon_broken{0};

    // Signed fill deltas with the venue's own timestamp, kept long enough to
    // rewind the book to any snapshot still worth comparing against. A venue
    // snapshot describes an instant; the book has moved since. Comparing the
    // two directly reports a break on every instrument that traded, which is
    // an alarm that fires whenever the system is working.
    struct FillDelta {
        std::uint64_t ts_ns{0};
        std::int64_t lots{0};
    };

    std::unordered_map<std::uint64_t, std::vector<FillDelta>> fills_after;
    // Which snapshot first disagreed about an instrument. A fill that lands in
    // the round trip around the venue's own timestamp falls on the wrong side
    // of the rewind boundary and disagrees exactly once; a real break survives
    // the next independent pull. Halting on the first would mean a busy
    // instrument freezes itself on network jitter, and the halt does not clear.
    std::unordered_map<std::uint64_t, std::uint64_t> break_since;

    void record_fill(InstrumentId instrument, Side side, QtyLots delta, UnixNanos ts) {
        fills_after[instrument.packed].push_back(
            {.ts_ns = ts.ns, .lots = side == Side::Buy ? delta.raw : -delta.raw});
    }

    std::unordered_map<std::uint64_t, std::uint64_t> inflight_; // coid -> submit mono ns
    observ::Histogram submit_hist;

private:
public:
    // The ledger is written HERE, by the process that did the arithmetic, and
    // is never re-derived downstream from the execution rows: a re-derivation
    // depends on instrument data that can change under it, so it would answer
    // a question about today with numbers from today rather than from the fill.
    void journal_pnl(const OrderEvent& event,
                     const InstrumentBase& base,
                     raikiri_journal::types::pnlKindEnum kind,
                     FixedE9 amount) noexcept {
        if (amount.raw == 0 || amount.is_undef()) {
            return; // an increment of nothing is not a fact worth keeping
        }
        (void)schema::append_msg<msgs::PnlIncrementMsg>(*journal_, event.ts_event.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.kind(kind);
            m.settleSide(portfolio::settle_side(base.kind) == portfolio::SettleSide::Base
                             ? jt::settleSideEnum::Base
                             : jt::settleSideEnum::Quote);
            m.strategyId(event.client_order_id.strategy_id());
            m.instrument(jt::instrumentId{event.instrument.packed});
            m.symbol().assign_range(base.exchange_symbol.view());
            m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
            // The fill's identity, not necessarily the venue's id: on a venue
            // that mints none this is what keeps two increments apart, and it
            // is written rather than re-derived downstream because
            // PnlIncrementMsg carries no leaves to derive it from.
            m.execIdField().assign_range(
                fill_identity(event.client_order_id, event.leaves_qty, event.exec_id.view())
                    .view());
            m.amount(jt::fixedE9{amount.raw});
            m.tsEvent(jt::timestampNs{event.ts_event.ns});
        });
    }

    // The book after a fill moved it. Written HERE for the same reason the
    // ledger is: this process owns the arithmetic, and a second one folding
    // the same fills would answer the same question differently. Downstream
    // — admind, persisterd, and this process's own restart — read the row
    // rather than re-deriving it.
    void journal_position(const portfolio::Position& position,
                          const InstrumentBase& base) noexcept {
        (void)schema::append_msg<msgs::PositionMsg>(
            *journal_, position.ts_updated().ns, [&](auto m) {
                namespace jt = raikiri_journal::types;
                m.strategyId(position.key().strategy_id);
                m.settleSide(portfolio::settle_side(base.kind) == portfolio::SettleSide::Base
                                 ? jt::settleSideEnum::Base
                                 : jt::settleSideEnum::Quote);
                m.venue(static_cast<jt::venueIdEnum>(position.key().instrument.venue()));
                m.instrument(jt::instrumentId{position.key().instrument.packed});
                m.symbol().assign_range(base.exchange_symbol.view());
                m.netQty(jt::qtyLots{position.qty().raw});
                // Cost, never the average: an average is cost over size and is
                // recomputed whenever it is asked for.
                m.cost(jt::fixedE9{position.cost().raw});
                m.realized(jt::fixedE9{position.realized().raw});
                m.fees(jt::fixedE9{position.fees().raw});
                m.fillCount(position.fill_count());
                m.tsOpened(jt::timestampNs{position.ts_opened().ns});
                m.tsUpdated(jt::timestampNs{position.ts_updated().ns});
            });
    }

private:
    void journal_cmd(const OrderCmd& cmd) noexcept {
        (void)schema::append_msg<msgs::OrderCmdMsg>(*journal_, cmd.ts_created.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.kind(static_cast<jt::cmdKindEnum>(cmd.kind));
            m.side(static_cast<jt::sideEnum>(cmd.side));
            m.orderType(static_cast<jt::orderTypeEnum>(cmd.order_type));
            m.tif(static_cast<jt::tifEnum>(cmd.tif));
            m.flags(static_cast<std::uint8_t>(cmd.flags));
            m.instrument(jt::instrumentId{cmd.instrument.packed});
            m.clientOrderIdField(jt::clientOrderId{cmd.client_order_id.raw});
            m.exchangeClientOrderIdField().assign_range(cmd.exchange_client_order_id.view());
            m.exchangeOrderIdField().assign_range(cmd.target_exchange_order_id.view());
            m.price(jt::priceTicks{cmd.price.raw});
            m.qty(jt::qtyLots{cmd.qty.raw});
            m.tsCreated(jt::timestampNs{cmd.ts_created.ns});
        });
    }

    std::vector<Gateway>* gateways_;
    oms::OrderStore* store_;
    oms::RiskGate* risk_;
    oms::IdGen* ids_;
    journal::JournalWriter* journal_;
    const Clock* clock_;
};

} // namespace

// How long a published snapshot stays worth comparing against. The gateway
// pulls every 30s; anything older than two rounds means it stopped answering,
// and a comparison against a number from five minutes ago is not a check.
constexpr std::uint64_t k_snapshot_max_age_ns = 90ULL * 1'000'000'000ULL;

// Read every 5s. The pull behind it is far slower, but a break should surface
// on the first loop after it becomes visible rather than on the next pull.
constexpr std::uint64_t k_recon_read_interval_ns = 5ULL * 1'000'000'000ULL;

// A break is journalled, not just printed: stderr scrolls away, and the one
// question asked afterwards is always "when did it start", which only a
// durable row can answer.
void journal_break(raikiri::journal::JournalWriter& journal,
                   raikiri::VenueId venue,
                   raikiri::InstrumentId instrument,
                   std::string_view symbol,
                   const raikiri::portfolio::ReconResult& result,
                   raikiri::UnixNanos now) {
    (void)raikiri::schema::append_msg<msgs::ReconBreakMsg>(journal, now.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.verdict(result.verdict == raikiri::portfolio::ReconVerdict::Unknown
                      ? jt::reconVerdictEnum::Unknown
                      : jt::reconVerdictEnum::Break);
        m.venue(static_cast<jt::venueIdEnum>(venue));
        m.instrument(jt::instrumentId{instrument.packed});
        m.symbol().assign_range(symbol);
        m.localQty(jt::qtyLots{result.local_qty.raw});
        m.venueQty(jt::qtyLots{result.venue_qty.raw});
        m.tsSeen(jt::timestampNs{now.ns});
    });
}

// Compares what the venue published against the book, and halts what disagrees.
//
// This runs HERE and not in the gateway because this process owns the book.
// The gateway sees the same event stream but has to re-derive a position from
// it, and its rule for "how much filled" was not this one — so the venue got
// compared against a number nobody was trading on. The gateway now publishes
// only what the venue said, and the arithmetic happens once, where the answer
// already exists.
//
// The book is split by strategy and the venue is not, so the local side is the
// sum across strategies. One gateway has exactly one command producer — the
// rings are SPSC — so that sum is the whole book for this venue, not a share
// of it.
// Orders the book still holds open that the venue no longer has.
//
// THE ANSWER TO SILENCE IS NOT "CLOSED". A stale table is a venue that did not
// answer, and closing a desk's book on that basis is a far worse error than
// carrying a dead order for another interval — so a pass that is not fresh
// closes nothing at all.
//
// AND A YOUNG ORDER IS NOT A MISSING ONE. The venue answered at some instant;
// an order sent after that instant was never in its list and is absent for the
// most ordinary reason there is. Only orders older than the pull are judged.
void reconcile_open_orders(KernelContext& ctx,
                           raikiri::VenueId venue,
                           const raikiri::shm::OpenOrderTable& table,
                           raikiri::journal::JournalWriter& journal,
                           raikiri::UnixNanos now) {
    using namespace raikiri;

    std::unordered_set<std::string_view> at_venue;
    UnixNanos pulled{};
    for (std::size_t idx = 0; idx < shm::k_open_order_cells; ++idx) {
        shm::OpenOrderCell cell{};
        if (table.read(idx, cell) == 0 || cell.ts_pulled.ns == 0) {
            continue;
        }
        if (now.ns - cell.ts_pulled.ns > k_snapshot_max_age_ns) {
            continue; // last pass has aged out
        }
        pulled = UnixNanos{std::max(pulled.ns, cell.ts_pulled.ns)};
        at_venue.insert(cell.exchange_client_order_id.view());
    }
    if (pulled.ns == 0) {
        return; // the venue said nothing recent; this is Unknown, not empty
    }

    std::vector<const oms::Order*> closed;
    ctx.orders().for_each_open([&](const oms::Order& order) {
        if (order.is_terminal() || order.instrument().venue() != venue) {
            return;
        }
        if (at_venue.contains(order.exchange_client_order_id().view())) {
            return;
        }
        // `ts_created` is when this process minted the order, which is the
        // earliest it could possibly have reached the venue. Anything younger
        // than the pull was never in that list and is absent for the most
        // ordinary reason there is.
        if (order.ts_created().ns == 0 || order.ts_created().ns > pulled.ns) {
            return;
        }
        closed.push_back(&order);
    });

    for (const auto* order : closed) {
        ++ctx.orders_closed_by_recon;
        RLOG_WARN("strategyd: RECON order {} is open here and gone at {} — closing",
                  order->exchange_client_order_id().view(),
                  to_string(venue));
        OrderEvent event{};
        event.type = OrderEventType::ReconClosed;
        event.instrument = order->instrument();
        event.side = order->side();
        event.client_order_id = order->client_order_id();
        event.exchange_client_order_id = order->exchange_client_order_id();
        event.qty = order->qty();
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = now;
        event.ts_init = now;
        (void)ctx.orders().apply_event(event);
        (void)journal;
    }
}

void reconcile_against_venue(KernelContext& ctx,
                             raikiri::VenueId venue,
                             const raikiri::shm::CatalogueTable& catalogue,
                             const raikiri::shm::PositionTable& table,
                             raikiri::journal::JournalWriter& journal,
                             raikiri::UnixNanos now) {
    using namespace raikiri;

    std::unordered_map<std::uint64_t, std::int64_t> book;
    for (const auto& held : ctx.positions.all()) {
        book[held.key().instrument.packed] += held.qty().raw;
    }
    // Fills older than any snapshot we would still compare against can never
    // be rewound past again.
    for (auto& [packed, deltas] : ctx.fills_after) {
        const auto cutoff = now.ns > k_snapshot_max_age_ns ? now.ns - k_snapshot_max_age_ns : 0;
        std::erase_if(deltas, [cutoff](const auto& d) { return d.ts_ns < cutoff; });
    }

    for (const auto& listed : catalogue.all()) {
        const auto id = base_of(listed).id;
        const auto idx = id.symbol().raw;
        if (idx >= shm::k_position_cells) {
            continue;
        }
        shm::PositionCell cell{};
        const bool written = table.read(idx, cell) != 0;
        // The book AS OF THE SNAPSHOT, not as of now: every fill the venue had
        // not yet seen when it answered is rewound back out. The venue's own
        // stamp is the boundary where it gave one, since the fills being
        // rewound carry venue timestamps too and the two clocks must be the
        // same clock for the subtraction to mean anything.
        const auto boundary =
            cell.report.ts_venue.ns != 0 ? cell.report.ts_venue.ns : cell.ts_pulled.ns;
        auto local_raw = book.contains(id.packed) ? book[id.packed] : 0;
        if (const auto it = ctx.fills_after.find(id.packed); it != ctx.fills_after.end()) {
            for (const auto& delta : it->second) {
                if (delta.ts_ns > boundary) {
                    local_raw -= delta.lots;
                }
            }
        }
        const auto local = QtyLots::from_raw(local_raw);
        // Never written, or written and gone stale: the venue has said nothing
        // recent. That is Unknown, and Unknown about a flat instrument is not
        // worth a line — we are not holding anything for it to be wrong about.
        const bool answered = written && !cell.report.net_qty.is_undef() &&
                              cell.ts_pulled.ns != 0 &&
                              now.ns - cell.ts_pulled.ns <= k_snapshot_max_age_ns;
        if (!answered && local.raw == 0 && (!written || cell.report.net_qty.raw == 0)) {
            continue;
        }

        std::optional<PositionReport> report;
        if (answered && cell.report.net_qty.raw != 0) {
            report = cell.report;
        }
        const auto result = portfolio::reconcile_position(answered, report, local);
        switch (result.verdict) {
        case portfolio::ReconVerdict::Agree:
            ++ctx.recon_agreed;
            break;
        case portfolio::ReconVerdict::Unknown:
            ++ctx.recon_unknown;
            break;
        case portfolio::ReconVerdict::Break:
            ++ctx.recon_broken;
            break;
        }
        bool confirmed = false;
        if (result.verdict == portfolio::ReconVerdict::Break) {
            const auto first = ctx.break_since.find(id.packed);
            if (first == ctx.break_since.end()) {
                ctx.break_since.emplace(id.packed, boundary);
            } else if (first->second != boundary) {
                confirmed = true;
                ctx.halted.insert(id.packed);
            }
        } else if (result.verdict == portfolio::ReconVerdict::Agree) {
            ctx.break_since.erase(id.packed);
        }
        const auto seen = ctx.recon_seen.find(id.packed);
        const bool changed = seen == ctx.recon_seen.end() || seen->second != result.verdict;
        ctx.recon_seen[id.packed] = result.verdict;
        // The moment the halt engages is the one an operator needs, and it is
        // the SECOND break — where the verdict has not changed — so it has to
        // survive the gate that keeps a standing condition from repeating.
        if (result.verdict == portfolio::ReconVerdict::Agree || (!changed && !confirmed)) {
            continue;
        }
        journal_break(journal, venue, id, base_of(listed).exchange_symbol.view(), result, now);
        // A halt is the loudest thing this process does on its own, so it gets
        // the level someone greps for rather than the one every break shares.
        if (confirmed) {
            RLOG_ERROR("strategyd: RECON {} (halted) {} local={} venue={} — {}",
                       portfolio::to_string(result.verdict),
                       base_of(listed).exchange_symbol.view(),
                       result.local_qty.raw,
                       result.venue_qty.raw,
                       result.detail);
        } else {
            RLOG_WARN("strategyd: RECON {} {} local={} venue={} — {}",
                      portfolio::to_string(result.verdict),
                      base_of(listed).exchange_symbol.view(),
                      result.local_qty.raw,
                      result.venue_qty.raw,
                      result.detail);
        }
    }
}

// Rebuilds the book from this process's own journal before it writes another
// line to it.
//
// Without this a restart came up FLAT while the venue still held everything,
// and the only thing that noticed was reconciliation — correctly, by halting
// every instrument as a break. That is the right alarm for a real
// disagreement and the wrong one for an ordinary restart.
//
// It replays PositionMsg only, and takes the last row per key rather than
// folding fills: the rows are already this process's own arithmetic, so
// reading them back is not a second derivation of anything. Order events are
// deliberately NOT replayed — the venue is authoritative on what is still
// open, and reconciliation is what asks it.
//
// EVERY attached catalogue, not the first one. A position is restorable when
// the gateway that lists its instrument is attached, and with more than one
// gateway that is not a question a single catalogue can answer: consulting
// only the first silently dropped every position on every other market, and
// the kernel then came up flat on holdings the venue still had — which
// reconciliation correctly halts, one instrument at a time, for a reason
// nobody would look for here.
struct Restoration {
    std::size_t restored{0};
    // Rows whose gateway is not attached. Not an error — running against a
    // subset of gateways is ordinary — but never silent, because the kernel is
    // flat on them and something has to say so.
    std::vector<std::string> unattached;
};

// One row of the ledger for a funding charge, on one strategy's book.
void write_funding_row(journal::JournalWriter& writer,
                       const LedgerEntry& charge,
                       const InstrumentBase& base,
                       std::uint16_t strategy_id,
                       FixedE9 amount) noexcept {
    if (amount.raw == 0) {
        return;
    }
    (void)schema::append_msg<msgs::PnlIncrementMsg>(writer, charge.settled_at.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.kind(jt::pnlKindEnum::Funding);
        m.settleSide(portfolio::settle_side(base.kind) == portfolio::SettleSide::Base
                         ? jt::settleSideEnum::Base
                         : jt::settleSideEnum::Quote);
        m.strategyId(strategy_id);
        m.instrument(jt::instrumentId{base.id.packed});
        m.symbol().assign_range(base.exchange_symbol.view());
        // No order produced this, so there is no client order id to name — zero
        // here means "not from an order", not "order zero".
        m.clientOrderIdField(jt::clientOrderId{0});
        // NOT the venue's own id for the charge. One settlement reaches this
        // process twice — live off the private socket and again from the REST
        // ledger the poller reconciles against — and the venue mints a
        // DIFFERENT id on each. Keying on either would book the money twice.
        //
        // The identity that both legs agree on is the settlement itself:
        // symbol and the second it landed on, plus the book it was split onto.
        // Seconds rather than nanoseconds because the two sources state the
        // stamp at different resolutions, and no instrument settles twice in
        // one second.
        char buf[48];
        const auto symbol = base.exchange_symbol.view();
        auto* out = std::copy(symbol.begin(), symbol.end(), buf);
        *out++ = ':';
        auto [tick, tick_ec] =
            std::to_chars(out, buf + sizeof buf, charge.settled_at.ns / 1'000'000'000);
        out = tick_ec == std::errc{} ? tick : out;
        *out++ = ':';
        const auto [end, ec] = std::to_chars(out, buf + sizeof buf, strategy_id);
        const auto len = ec == std::errc{} ? static_cast<std::size_t>(end - buf)
                                           : static_cast<std::size_t>(out - buf);
        m.execIdField().assign_range(std::string_view{buf, len});
        m.amount(jt::fixedE9{amount.raw});
        m.tsEvent(jt::timestampNs{charge.settled_at.ns});
    });
}

// A funding charge, split across whoever was carrying the instrument.
//
// THE VENUE CHARGES THE NETTED INSTRUMENT; the book is keyed by (strategy,
// instrument). Where two strategies share one instrument the charge is
// pro-rated by the size each carries — an approximation, and named as one: the
// split uses the size held NOW, not at the settlement stamp, because a
// per-strategy history at the venue's settlement resolution does not exist.
//
// A charge nobody is holding is still money that moved, so it lands on the
// manual book rather than being dropped. Dropping it would put the ledger
// permanently at odds with the venue's balance.
void journal_funding(journal::JournalWriter& writer,
                     const portfolio::PositionBook& book,
                     const LedgerEntry& charge,
                     const InstrumentBase& base) noexcept {
    if (charge.amount.raw == 0 || charge.amount.is_undef()) {
        return;
    }
    std::int64_t total_lots = 0;
    const portfolio::Position* last = nullptr;
    for (const auto& position : book.all()) {
        if (position.key().instrument != charge.instrument || position.qty().raw == 0) {
            continue;
        }
        total_lots += std::abs(position.qty().raw);
        last = &position;
    }
    if (total_lots == 0) {
        write_funding_row(writer, charge, base, k_manual_strategy_id, charge.amount);
        return;
    }
    // The remainder goes to the last holder so the parts sum to the charge
    // exactly; rounding each share on its own loses cents into nobody's book.
    std::int64_t paid = 0;
    for (const auto& position : book.all()) {
        if (position.key().instrument != charge.instrument || position.qty().raw == 0) {
            continue;
        }
        const auto share = &position == last
                               ? charge.amount.raw - paid
                               : charge.amount.raw * std::abs(position.qty().raw) / total_lots;
        paid += share;
        write_funding_row(writer, charge, base, position.key().strategy_id, FixedE9{share});
    }
}

Restoration restore_book(portfolio::PositionBook& book,
                         std::span<const shm::CatalogueTable* const> catalogues,
                         const std::string& dir) {
    auto reader = journal::JournalReader::open(dir, "journal");
    if (!reader.has_value()) {
        return {}; // no journal yet: a first run, not a loss
    }

    // Copied out of the frame rather than kept as a view: a decoded message
    // points into the journal's mapping, and the map outlives the walk.
    struct Row {
        std::uint16_t strategy_id{0};
        InstrumentId instrument{};
        portfolio::Position::Restored state{};
    };

    std::unordered_map<std::uint64_t, Row> latest;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        if (frame.schema_id != schema::message_id<msgs::PositionMsg>()) {
            continue;
        }
        const auto m = schema::view_msg<msgs::PositionMsg>(frame.payload);
        const InstrumentId id{m.instrument().value()};
        Row row{};
        row.strategy_id = m.strategyId().value();
        row.instrument = id;
        row.state = {.qty = QtyLots::from_raw(m.netQty().value()),
                     .cost = FixedE9::from_raw(m.cost().value()),
                     .realized = FixedE9::from_raw(m.realized().value()),
                     .fees = FixedE9::from_raw(m.fees().value()),
                     .fill_count = m.fillCount().value(),
                     .ts_opened = UnixNanos{m.tsOpened().value()},
                     .ts_updated = UnixNanos{m.tsUpdated().value()}};
        // Last write wins, which is what "the book as it stands" means.
        latest[(static_cast<std::uint64_t>(row.strategy_id) << 48) ^ id.packed] = row;
    }

    Restoration out{};
    std::set<std::string> unattached;
    for (const auto& [key, row] : latest) {
        if (row.state.qty.raw == 0 && row.state.realized.raw == 0) {
            continue; // a closed position carries nothing worth restoring
        }
        const bool listed = std::ranges::any_of(catalogues, [&](const auto* table) {
            return table != nullptr && table->find(row.instrument) != nullptr;
        });
        if (!listed) {
            // Named by account rather than counted, so the line says which
            // gateway to start rather than that some number of them is missing.
            std::array<char, 16> buf{};
            unattached.emplace(format_account(
                {row.instrument.venue(), row.instrument.market()}, buf.data(), buf.size()));
            continue;
        }
        if (book.restore({.strategy_id = row.strategy_id, .instrument = row.instrument},
                         row.state)) {
            ++out.restored;
        }
    }
    out.unattached.assign(unattached.begin(), unattached.end());
    return out;
}

namespace {
[[nodiscard]] constexpr std::string_view reject_name(strategy::RejectReason why) noexcept {
    switch (why) {
    case strategy::RejectReason::Risk:
        return "risk gate";
    case strategy::RejectReason::Capacity:
        return "order store full";
    case strategy::RejectReason::VenueDown:
        return "no gateway for this instrument, or its command ring is full";
    }
    return "unknown";
}
} // namespace

int main(int argc, char** argv) {
    const raikiri::log::Scope logging{"strategyd"};
    // Stdout carries the end-of-run report and nothing else; the logger writes
    // to stderr. Line buffered so that a run redirected with 2>&1 keeps the
    // report in the right place relative to the records around it.
    (void)std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::uint64_t duration_s = 10;
    // Account names, which are also the segment names: "bybit" is bybit perp,
    // "bybit-spot" is the spot gateway beside it. Repeatable, because one
    // kernel holds one book across every market it trades.
    std::vector<std::string> wanted;
    // Names this strategy's book and rides in the top 16 bits of every
    // ClientOrderId, so it reaches the ledger without extra plumbing. Two
    // strategies that share it net into one position; distinct ones do not.
    std::uint16_t strategy_id = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--duration" && i + 1 < argc) {
            duration_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg.starts_with("--venue=") || arg.starts_with("--gateway=")) {
            wanted.emplace_back(arg.substr(arg.find('=') + 1));
        } else if (arg.starts_with("--strategy-id=")) {
            strategy_id = static_cast<std::uint16_t>(std::atoi(arg.substr(14).data()));
        }
    }
    if (wanted.empty()) {
        wanted.emplace_back("sim");
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;

    // Attach every requested gateway. Reserved up front: the readers and
    // producers below hold pointers into shm, not into this vector, but a
    // reallocation would still move segment clients that own mappings.
    std::vector<Gateway> gateways;
    gateways.reserve(wanted.size());
    for (const auto& name : wanted) {
        const auto account = account_from_string(name);
        if (account.venue == VenueId::Invalid) {
            fmt::print(stderr, "strategyd: unknown gateway {}\n", name);
            return 2;
        }
        for (const auto& existing : gateways) {
            if (existing.account == account) {
                fmt::print(stderr, "strategyd: {} named twice\n", name);
                return 2;
            }
        }
        // Retry: the gateway may still be coming up.
        const auto seg_path = shm::gateway_segment_path(account);
        std::expected<shm::SegmentClient, shm::SegError> client =
            std::unexpected{shm::SegError::MapFailed};
        for (int attempt = 0; attempt < 100; ++attempt) {
            client = shm::SegmentClient::attach(seg_path);
            if (client.has_value()) {
                break;
            }
            ::usleep(50'000);
        }
        if (!client.has_value()) {
            RLOG_ERROR("strategyd: cannot attach {}", seg_path);
            return 1;
        }
        auto channels = shm::open_gateway_channels(*client);
        if (!channels.has_value()) {
            RLOG_ERROR("strategyd: channel open failed on {} ({})",
                       seg_path,
                       static_cast<int>(channels.error()));
            return 1;
        }
        (void)client->register_reader(::getpid());
        gateways.emplace_back(account, name, std::move(*client), *channels);
    }

    const char* data_dir_env = std::getenv("RAIKIRI_DATA_DIR");
    const std::string data_dir = data_dir_env != nullptr ? data_dir_env : "raikiri-data";
    auto journal_writer = journal::JournalWriter::open(data_dir + "/journal/strategyd", "journal");
    if (!journal_writer.has_value()) {
        RLOG_ERROR("strategyd: journal open failed");
        return 1;
    }

    oms::OrderStore store{4096};
    // 500 bp — five per cent off the mid. Wide enough that a limit resting a
    // long way out is still allowed, narrow enough that a misplaced decimal
    // never reaches a venue.
    oms::RiskGate risk{{.max_open_orders = 64,
                        .max_qty_lots = QtyLots::from_raw(1'000),
                        .max_price_band_bp = 500}};
    oms::IdGen ids{strategy_id, /*seed=*/0x7A10ULL};
    KernelContext ctx{gateways, store, risk, ids, *journal_writer, clock};

    std::size_t instrument_count = 0;
    for (const auto& gw : gateways) {
        instrument_count += gw.ch.catalogue->size();
    }
    if (instrument_count == 0) {
        RLOG_ERROR("strategyd: every attached gateway published an empty catalogue");
        return 1;
    }

    // DemoMaker quotes both sides, which is only ever appropriate against the
    // sim venue. A live gateway is observed, not traded: the loop, the
    // journal and the latency table are identical either way.
    // Before a single new line is written to the journal: what this process
    // held when it last stopped.
    std::vector<const shm::CatalogueTable*> catalogues;
    catalogues.reserve(gateways.size());
    for (const auto& gw : gateways) {
        catalogues.push_back(gw.ch.catalogue);
    }
    const auto restoration =
        restore_book(ctx.positions, catalogues, data_dir + "/journal/strategyd");
    if (restoration.restored > 0) {
        RLOG_INFO("strategyd: restored {} position(s) from the journal", restoration.restored);
    }
    if (!restoration.unattached.empty()) {
        std::string accounts;
        for (const auto& name : restoration.unattached) {
            accounts += (accounts.empty() ? "" : ", ") + name;
        }
        RLOG_WARN("strategyd: the journal holds positions on {} and no attached gateway lists "
                  "them — this process is flat on those and will not reconcile them",
                  accounts);
    }

    strategies::DemoMaker maker;
    Observer observer;
    const bool quoting = gateways.size() == 1 && gateways.front().account.venue == VenueId::Sim;
    strategy::Strategy& active = quoting ? static_cast<strategy::Strategy&>(maker) : observer;
    std::string attached;
    for (const auto& gw : gateways) {
        attached += (attached.empty() ? "" : ", ") + gw.name;
    }
    RLOG_INFO("strategyd: attached to {}, {} instruments, running {} for {}s",
              attached,
              instrument_count,
              quoting ? "DemoMaker" : "observer",
              duration_s);
    active.on_start(ctx);

    observ::Histogram tick_hist; // BBO ts_init -> callback dispatch
    observ::Histogram rt_hist;   // submit -> first venue verdict
    std::uint64_t exec_overruns = 0;
    std::uint64_t unattributed_fills = 0;
    std::uint64_t funding_overruns = 0;
    std::uint64_t funding_charges = 0;
    std::uint64_t fills = 0;
    std::uint64_t bad_transitions = 0;   // FSM refusals: must stay 0
    std::uint64_t position_refusals = 0; // position book full: must stay 0
    std::uint64_t funding_ticks = 0;
    std::uint64_t desk_intents = 0;  // hand-placed intents seen
    std::uint64_t desk_refusals = 0; // and how many never reached a venue

    for (auto& gw : gateways) {
        gw.recon_due = clock.mono().ns; // once at start, then on the interval
    }

    // Watch the OWNER, not the generation. A restarting gateway unlinks the
    // segment and creates a new file, so this process keeps its mapping of the
    // old inode alive and the generation it can see never changes — while the
    // new gateway serves a different file entirely. What does change is the
    // heartbeat in the mapping we hold: nobody stamps it any more.
    //
    // When that happens every ring cursor, every instrument id and the whole
    // order store belong to a world that no longer exists, and commands pushed
    // into these rings reach nobody. There is no sound way to keep going, so
    // this stops and says why.
    std::string gateway_gone;

    const auto t_end = clock.mono() + duration_s * 1'000'000'000ULL;
    while (!g_stop.load(std::memory_order_relaxed) && clock.mono() < t_end) {
        // Verdicts drain before market data, across EVERY gateway: a strategy
        // must never act on a price tick while its own order state is a poll
        // behind, and the order it is behind on may be on another venue.
        // A person's intents drain FIRST, ahead of verdicts and market data.
        //
        // Someone watching the screen has already decided; the cost of holding
        // their order for one more poll is a tick of slippage they did not ask
        // for. Nothing downstream is skipped by going first — the desk hands
        // each intent to the same submit path a strategy uses, so the risk
        // gate, the store, the journal and the halt all still see it.
        OrderCmd intent{};
        for (auto& gw : gateways) {
            while (gw.intents.try_pop(intent)) {
                desk_intents += 1;
                // AN INTENT MAY ONLY SPEAK FOR THE DESK. The id packs the
                // strategy that owns the order, so an intent carrying anyone
                // else's is a caller trying to place orders in a strategy's
                // name — refused here rather than trusted because it arrived
                // over a loopback socket.
                if (intent.client_order_id.strategy_id() != k_manual_strategy_id) {
                    desk_refusals += 1;
                    continue;
                }
                switch (intent.kind) {
                case CmdKind::Create: {
                    const strategy::NewOrder order{.instrument = intent.instrument,
                                                   .side = intent.side,
                                                   .type = intent.order_type,
                                                   .tif = intent.tif,
                                                   .flags = intent.flags,
                                                   .price = intent.price,
                                                   .qty = intent.qty};
                    const auto placed = ctx.submit_as(order, intent.client_order_id);
                    if (!placed.has_value()) {
                        desk_refusals += 1;
                        // WHY, not just that. A desk that refuses in silence is
                        // a desk nobody can debug: from the caller's side a
                        // rejected order and a lost one look identical.
                        RLOG_WARN("strategyd: desk order {} refused — {}",
                                  intent.client_order_id.raw,
                                  reject_name(placed.error()));
                    }
                    break;
                }
                case CmdKind::Cancel:
                    if (!ctx.cancel(intent.client_order_id)) {
                        desk_refusals += 1;
                        RLOG_WARN("strategyd: desk cancel {} refused — no such order here",
                                  intent.client_order_id.raw);
                    }
                    break;
                case CmdKind::Amend:
                    // The desk does not amend. A person changing their mind
                    // cancels and re-places, which is two decisions they can
                    // see rather than one the venue may or may not support.
                    desk_refusals += 1;
                    break;
                }
            }
        }

        // Funding first: a charge settled before the fills that follow it in
        // this pass, and the ledger reads better in the order money moved.
        shm::LedgerEntryRec charge_rec{};
        for (auto& gw : gateways) {
            for (;;) {
                const auto poll = gw.charges.poll(charge_rec);
                if (poll == shm::PollResult::Empty) {
                    break;
                }
                if (poll == shm::PollResult::Overrun) {
                    ++funding_overruns; // a lost charge is lost money: worth counting
                    continue;
                }
                const auto& charge = charge_rec.charge;
                const auto* instrument = gw.ch.catalogue->find(charge.instrument);
                if (instrument == nullptr) {
                    continue; // an id from a catalogue this process never read
                }
                // THE TWO KINDS ARE NOT BOOKED THE SAME WAY. Funding has no
                // other source, so it is written. A fee was already written
                // from the fill that incurred it, and writing this copy too
                // would charge every commission twice — so it is compared.
                if (charge.kind == LedgerKind::Funding) {
                    journal_funding(*journal_writer, ctx.positions, charge, base_of(*instrument));
                    ++funding_charges;
                    continue;
                }
                ctx.reconcile_fee(charge, base_of(*instrument));
            }
        }

        shm::ExecEventRec rec{};
        for (auto& gw : gateways) {
            for (;;) {
                const auto poll = gw.exec.poll(rec);
                if (poll == shm::PollResult::Empty) {
                    break;
                }
                if (poll == shm::PollResult::Overrun) {
                    ++exec_overruns; // policy: halt+reconcile; demo just counts
                    continue;
                }
                // A COPY, because the id has to be restored onto it. A venue
                // event names the order by OUR exchange id and leaves
                // client_order_id at zero; everything downstream — the position
                // book's attribution, the ledger's strategy column, the fill's
                // dedup key, the round-trip clock — reads the strategy out of
                // that id. Left at zero they all quietly say "strategy 0".
                OrderEvent event = rec.event;
                if (const auto it = ctx.inflight_.find(event.client_order_id.raw);
                    it != ctx.inflight_.end()) {
                    rt_hist.record(clock.mono().ns - it->second);
                    ctx.inflight_.erase(it);
                }
                // Read leaves BEFORE the FSM moves it: the fill quantity is the
                // drop in leaves, never the event's own qty. A venue that resends
                // a fill notification then derives zero and moves no money.
                //
                // FOUND THE WAY apply_event ROUTES, which is not by our own id.
                // A venue knows an order only by the id we sent it, so every
                // fill off a private stream carries client_order_id ZERO and a
                // lookup by it finds nothing — leaving prev_leaves at zero, a
                // delta of zero, and the whole booking below skipped in
                // silence. The order store already resolves either id; this has
                // to resolve the same one or the book never learns what the
                // store just did.
                QtyLots prev_leaves = QtyLots::from_raw(0);
                const auto* order =
                    event.client_order_id.raw != 0 ? store.find(event.client_order_id) : nullptr;
                if (order == nullptr && !event.exchange_client_order_id.empty()) {
                    order = store.find(event.exchange_client_order_id);
                }
                if (order != nullptr) {
                    prev_leaves = order->leaves();
                    // Restored BEFORE anything reads it. The store knows which
                    // order this is; without this line the book, the ledger and
                    // the latency histogram each answer a question about it
                    // with a zero.
                    event.client_order_id = order->client_order_id();
                }
                if (const auto applied = store.apply_event(event);
                    !applied.has_value() && applied.error() == oms::ApplyError::BadTransition) {
                    ++bad_transitions;
                }
                if (event.type == OrderEventType::Filled) {
                    ++fills;
                    if (order == nullptr) {
                        // A fill for an order this process never had. Counted
                        // and said out loud: it is money that moved on an
                        // account we hold, and a silent one is the difference
                        // between a quiet desk and a broken one.
                        ++unattributed_fills;
                        RLOG_ERROR("strategyd: FILL {} matched no order here",
                                   event.exchange_client_order_id.view());
                    }
                    const auto delta = QtyLots::from_raw(prev_leaves.raw - event.leaves_qty.raw);
                    const auto* listed = ctx.instrument(event.instrument);
                    if (listed != nullptr && delta.raw > 0) {
                        const auto& base = base_of(*listed);
                        ctx.record_fill(event.instrument, event.side, delta, event.ts_event);
                        const auto applied = ctx.positions.on_fill(base, event, delta);
                        if (!applied.has_value()) {
                            ++position_refusals;
                        } else {
                            namespace jt = raikiri_journal::types;
                            // One increment per kind per exec_id: a flipped fill is
                            // still ONE realizing event to the venue, and the
                            // ledger's grain has to match the grain every
                            // reconciliation query joins on.
                            ctx.journal_pnl(
                                event, base, jt::pnlKindEnum::Realized, applied->realized);
                            // From what the book took, not from the event: a
                            // resent notification derives a zero delta, so
                            // apply_fill charges nothing, and reading the
                            // event's own fee here would put a row in the
                            // ledger that the book does not have. The two
                            // halves sum to the venue's fee exactly.
                            //
                            // NEGATED: the ledger's stated sign is "positive is
                            // money gained" and a fee is money leaving. The
                            // book keeps the opposite convention internally —
                            // net_realized() subtracts it — and this is the
                            // edge where the two meet.
                            const auto booked_fee = FixedE9::from_raw(
                                -(applied->close_fee.raw + applied->open_fee.raw));
                            ctx.journal_pnl(event, base, jt::pnlKindEnum::Fee, booked_fee);
                            // Remembered under the venue's own id for this
                            // execution, because that is what its ledger quotes
                            // back when it states the same fee.
                            if (booked_fee.raw != 0 && !event.exec_id.empty()) {
                                ctx.booked_fees.insert_or_assign(
                                    std::string{event.exec_id.view()},
                                    KernelContext::FeeMemo{.amount = booked_fee,
                                                           .booked_at = clock.wall()});
                            }
                            // After the increments, because the row is the
                            // state those increments produced.
                            if (const auto* held = ctx.positions.find(
                                    {event.client_order_id.strategy_id(), event.instrument});
                                held != nullptr) {
                                ctx.journal_position(*held, base);
                            }
                        }
                    }
                }
                active.on_order_event(ctx, event);
            }
        }
        shm::MdEvent md{};
        for (auto& gw : gateways) {
            while (gw.md.poll(md) == shm::PollResult::Item) {
                TradeTick trade{};
                trade.instrument = md.instrument;
                trade.price = md.price;
                trade.qty = md.qty;
                trade.aggressor = md.aggressor;
                trade.ts_event = md.ts_event;
                trade.ts_init = md.ts_init;
                active.on_trade(ctx, trade);
            }
        }
        for (auto& gw : gateways) {
            for (std::uint32_t sym = 0; sym < gw.bbo_seen.size(); ++sym) {
                const auto version = gw.ch.bbo->cells[sym].version_now();
                if (version == gw.bbo_seen[sym] || (version & 1U) != 0) {
                    continue;
                }
                Bbo bbo{};
                if (gw.ch.bbo->read(sym, bbo) != 0) {
                    gw.bbo_seen[sym] = version;
                    tick_hist.record(clock.wall().ns - bbo.ts_init.ns);
                    active.on_bbo(ctx, bbo);
                }
            }
            // Funding changes on the venue's schedule — eight hours on most
            // perps — so the gate matters more here than for BBO: without it
            // every poll would re-deliver the same rate thousands of times a
            // second.
            for (std::uint32_t sym = 0; sym < gw.funding_seen.size(); ++sym) {
                const auto version = gw.ch.funding->cells[sym].version_now();
                if (version == gw.funding_seen[sym] || (version & 1U) != 0) {
                    continue;
                }
                FundingUpdate funding{};
                if (gw.ch.funding->read(sym, funding) != 0) {
                    gw.funding_seen[sym] = version;
                    ++funding_ticks;
                    active.on_funding(ctx, funding);
                }
            }
        }
        const auto now_ns = clock.mono().ns;
        for (auto& timer : ctx.timers) {
            if (timer.deadline_ns != 0 && now_ns >= timer.deadline_ns) {
                timer.deadline_ns = 0;
                active.on_timer(ctx, timer.id);
            }
        }
        for (auto& gw : gateways) {
            // Monotonic, because that is the clock the gateway stamps the
            // heartbeat with; asking in wall time makes every owner look stale.
            if (!gw.client.owner_alive(now_ns)) {
                gateway_gone = gw.name;
                break;
            }
            if (now_ns >= gw.recon_due) {
                // Seven days is the widest window any venue's ledger reaches
                // back over, so a memo older than that will never be quoted.
                ctx.prune_booked_fees(clock.wall(), 7ULL * 24 * 60 * 60 * 1'000'000'000ULL);
                reconcile_open_orders(
                    ctx, gw.account.venue, *gw.ch.open_orders, *journal_writer, clock.wall());
                reconcile_against_venue(ctx,
                                        gw.account.venue,
                                        *gw.ch.catalogue,
                                        *gw.ch.positions,
                                        *journal_writer,
                                        clock.wall());
                gw.recon_due = now_ns + k_recon_read_interval_ns;
            }
        }
        if (!gateway_gone.empty()) {
            break;
        }
    }
    active.on_stop(ctx);
    journal_writer->flush();
    if (!gateway_gone.empty()) {
        RLOG_ERROR("strategyd: gateway {} stopped answering — cursors and book belong to a dead "
                   "generation, stopping",
                   gateway_gone);
    }

    const auto tick = tick_hist.snapshot();
    const auto submit = ctx.submit_hist.snapshot();
    const auto rt = rt_hist.snapshot();
    fmt::print("\n===== raikiri: {}, {}s, single unpinned core =====\n", attached, duration_s);
    fmt::print("{:<22}{:>10}{:>12}{:>12}{:>12}{:>12}\n",
               "metric (ns)",
               "count",
               "p50",
               "p90",
               "p99",
               "max");
    const auto row = [](const char* name, const observ::HistogramSnapshot& s) {
        fmt::print("{:<22}{:>10}{:>12}{:>12}{:>12}{:>12}\n",
                   name,
                   s.count,
                   s.p50_ns,
                   s.p90_ns,
                   s.p99_ns,
                   s.max_ns);
    };
    row("tick-to-callback", tick);
    row("submit-call", submit);
    row("order-round-trip", rt);
    fmt::print("fills={}  funding={}  open-at-exit={}  exec-ring-overruns={}  fsm-refusals={}  "
               "position-refusals={}\n",
               fills,
               funding_ticks,
               store.open_count(),
               exec_overruns,
               bad_transitions,
               position_refusals);
    // Refusals are reported beside the count, not hidden: a desk whose orders
    // are all being turned away looks exactly like a quiet desk otherwise.
    fmt::print("manual desk: intents={}  refused={}\n", desk_intents, desk_refusals);
    // Charges booked, and charges LOST. The second is money the ledger will
    // never carry, so it is printed even when it is zero.
    fmt::print("funding: charges-booked={}  ring-overruns={}\n", funding_charges, funding_overruns);
    fmt::print("fills-unattributed={}\n", unattributed_fills);
    fmt::print("recon: orders-closed={}\n", ctx.orders_closed_by_recon);
    // Unmatched is printed beside the other two on purpose: a reconciliation
    // that agrees on nothing because it joined on the wrong id looks identical
    // to one that had nothing to check.
    fmt::print("fees: agreed={}  disagreed={}  unmatched={}\n",
               ctx.fees_agreed,
               ctx.fees_disagreed,
               ctx.fees_unmatched);
    fmt::print("recon: agreed={}  unknown={}  broken={}  halted={}\n",
               ctx.recon_agreed,
               ctx.recon_unknown,
               ctx.recon_broken,
               ctx.halted.size());

    if (ctx.positions.size() > 0) {
        fmt::print("\n{:<16}{:>12}{:>16}{:>16}{:>16}\n",
                   "instrument",
                   "net (lots)",
                   "avg entry",
                   "realized",
                   "unrealized");
        char buf[32]{};
        for (const auto& position : ctx.positions.all()) {
            const auto* listed = ctx.instrument(position.key().instrument);
            if (listed == nullptr) {
                continue;
            }
            const auto& base = base_of(*listed);
            // Marked at the last BBO mid; an unrealized number is only ever as
            // good as the price the caller marks it against.
            auto mark = PriceTicks::from_raw(0);
            if (const auto bbo = ctx.bbo(position.key().instrument); bbo.has_value()) {
                mark = PriceTicks::from_raw((bbo->bid.raw + bbo->ask.raw) / 2);
            }
            const auto fixed = [&buf](FixedE9 v) -> std::string {
                return v.is_undef() ? std::string{"-"}
                                    : std::string{format_e9(v, buf, sizeof(buf))};
            };
            fmt::print("{:<16}{:>12}{:>16}{:>16}{:>16}\n",
                       base.exchange_symbol.view(),
                       position.qty().raw,
                       fixed(position.avg_entry_price(base)),
                       fixed(position.net_realized()),
                       mark.raw == 0 ? std::string{"-"}
                                     : fixed(portfolio::unrealized_pnl(base, position, mark)));
        }
    }
    return gateway_gone.empty() ? 0 : 1;
}
