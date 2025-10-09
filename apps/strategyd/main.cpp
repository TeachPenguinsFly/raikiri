// strategyd — the strategy kernel (demo wiring).
//
//   strategyd [--gateway=<account>]... [--duration <sec>] [--strategy-id=<n>]
//             [--strategy <type>[:<id>[:<name>[:<config>]]]]... [--port <n>]
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
// --strategy is repeatable and names a type the process factory knows;
// `demo-maker` and `observer` are the two this repository carries, and a build
// that links others lists them at /v1/strategy-types without a line changing
// here. Everything past the id is the operator's: a name, then the whole
// remainder as an opaque config handed to the constructor verbatim.
// --strategy-id names the FIRST instance's id, and every --strategy that
// states none counts up from there.
//
// --port serves that control surface — list, start, stop, positions, pnl — on
// loopback. admind reaches it with `--upstream <name>=<port>` and forwards
// /v1/x/<name>/..., so nothing about a strategy needs naming in admind.
//
// Single thread, no locks: exec ring, md ring, BBO change-gates, timers, the
// control surface, all polled in one loop — the same shape the real kernel
// keeps forever. That single thread is what makes an HTTP handler calling
// on_start sound: it runs between dispatches, never inside one.

#include <raikiri/journal/detail.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/observ/histogram.hpp>
#include <raikiri/oms/attribution.hpp>
#include <raikiri/oms/fee_ledger.hpp>
#include <raikiri/oms/funding_identity.hpp>
#include <raikiri/oms/funding_split.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/kernel_gate.hpp>
#include <raikiri/oms/order_store.hpp>
#include <raikiri/oms/recon.hpp>
#include <raikiri/oms/risk.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/portfolio/reconcile.hpp>
#include <raikiri/portfolio/target.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/schema/codec.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/strategies/register.hpp>
#include <raikiri/strategy/control.hpp>
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/registry.hpp>
#include <raikiri/strategy/strategy.hpp>

#include <fmt/format.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

using msgs = raikiri_journal::schema::messages;
using namespace raikiri;

// Every Strategy callback defaults to a no-op, so observing a live venue
// needs no separate code path. Registered as a type like any other, because
// "watch this venue and place nothing" is a real thing to ask for rather than
// a special case of nothing being configured.
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
    // WHETHER THIS GATEWAY'S BOOK IS STILL TRUSTED. One machine per gateway on
    // purpose: a break on bybit-perp must not stop coinbase-spot, which shares
    // nothing with it but this process.
    oms::KernelGate kernel;

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

// The machinery every strategy's window looks through: the gateways, the order
// store, the risk gate, the book and the journal.
//
// NOT a strategy::Context, though it implements most of one. A Context is ONE
// strategy's view and every answer it gives means "yours" — the position, the
// orders it may cancel, the id it mints. This process runs several, so the
// window is InstanceContext below and this is what stands behind all of them.
// While the two were one type, "yours" meant one hardcoded strategy id, and a
// hand-placed order was risk-checked against a strategy's position.
class KernelContext {
public:
    KernelContext(const KernelContext&) = delete;
    KernelContext& operator=(const KernelContext&) = delete;

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
        // TWO HALTS, ONE FIELD. `recon_gate` halts a single instrument whose
        // venue disagreed; `kernel` halts a whole gateway whose picture is no
        // longer trusted. RiskGate already refuses on this flag, so neither
        // needs a reject code of its own.
        oms::Exposure exposure{.reconciliation_break =
                                   recon_gate.halted(order.instrument) || !gw->kernel.may_open(),
                               .open_orders = store_->open_count()};
        // THE BOOK OF WHOEVER IS PLACING IT. The id packs its owner, so a
        // hand-placed order is banded against the desk's position and a
        // strategy's against its own. Reading one fixed strategy's book here
        // priced every desk order off somebody else's exposure.
        if (const auto* held = positions.find({id.strategy_id(), order.instrument});
            held != nullptr) {
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

    bool cancel(ClientOrderId id) noexcept {
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

    std::optional<Bbo> bbo(InstrumentId instrument) const noexcept {
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

    const Instrument* instrument(InstrumentId id) const noexcept {
        const auto* gw = gateway_for(id);
        return gw != nullptr ? gw->ch.catalogue->find(id) : nullptr;
    }

    [[nodiscard]] const Clock& clock() const noexcept { return *clock_; }

    // --- kernel-side state the main loop reads ---
    portfolio::PositionBook positions;
    // Instruments this process has stopped opening on. Cleared only by a
    // restart, never by one agreeing pull: an automatic clear on a single
    // agreement re-opens the window the break was reporting.
    oms::ReconGate recon_gate;
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
    oms::FeeLedger fees;
    std::uint64_t fees_agreed{0};
    std::uint64_t fees_disagreed{0};
    std::uint64_t fees_unmatched{0};

    // The venue's own record of a fee, against what this process wrote.
    //
    // NEVER BOOKED. The fill already wrote it; this is the check that the two
    // agree, and a disagreement is a fact for a human rather than a correction
    // to apply automatically — the venue's number and ours are both evidence,
    // and silently overwriting one with the other loses the disagreement.
    // The verdict comes from the ledger; the saying-out-loud stays here,
    // because a thing that decides is testable and a thing that prints is not.
    void reconcile_fee(const LedgerEntry& charge, const InstrumentBase& base) {
        const auto checked = fees.reconcile(charge);
        switch (checked.verdict) {
        case oms::FeeVerdict::Agreed:
            ++fees_agreed;
            return;
        case oms::FeeVerdict::Disagreed:
            ++fees_disagreed;
            RLOG_ERROR("strategyd: FEE BREAK {} on {} — we booked {} e9, venue says {} e9",
                       charge.reference.view(),
                       base.exchange_symbol.view(),
                       checked.booked.raw,
                       charge.amount.raw);
            return;
        case oms::FeeVerdict::Unmatched:
            ++fees_unmatched;
            // Either a fill this process never saw, or the venue quotes a
            // different id here than on the execution — both worth a line,
            // because one is lost money and the other is a wrong join.
            RLOG_WARN("strategyd: FEE {} on {} — venue charged {}.{:09} against no fill we hold",
                      charge.reference.view(),
                      base.exchange_symbol.view(),
                      charge.amount.raw / 1'000'000'000,
                      std::abs(charge.amount.raw) % 1'000'000'000);
            return;
        }
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
    // Fills kept only long enough to rewind them out of a comparison against a
    // snapshot the venue took before it saw them — see oms/recon.hpp.
    std::unordered_map<std::uint64_t, std::vector<oms::BookedFill>> fills_after;

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

    // A REFUSAL THE PERSON WHO ASKED CAN FIND.
    //
    // The desk answers 202 with a client order id and hands the intent over;
    // if the risk gate then refuses it, `submit_as` returns before the store
    // and before the journal, and the order exists NOWHERE. The caller holds
    // an id for something that never happened, and the reason goes only to a
    // log they do not read — which is precisely the "a rejected order and a
    // lost one look identical" this file warns about two hundred lines down
    // and did not prevent.
    //
    // So write both halves the happy path writes: the command, so the order
    // exists at all, and a terminal event, so it is finished with. A replay
    // then puts it in Postgres and `/v1/orders/{id}` can answer.
    void journal_refusal(const OrderCmd& cmd, strategy::RejectReason why) noexcept {
        journal_cmd(cmd);
        (void)schema::append_msg<msgs::OrderEventMsg>(*journal_, clock_->wall().ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            // DENIED, NOT REJECTED. The domain separates them and the difference is
            // who refused: Rejected is the venue's word, and the venue never saw
            // this order. Writing Rejected here would blame a party that was
            // not asked.
            m.eventType(static_cast<jt::orderEventTypeEnum>(OrderEventType::Denied));
            m.side(static_cast<jt::sideEnum>(cmd.side));
            m.orderType(static_cast<jt::orderTypeEnum>(cmd.order_type));
            m.tif(static_cast<jt::tifEnum>(cmd.tif));
            m.flags(static_cast<std::uint8_t>(cmd.flags));
            m.instrument(jt::instrumentId{cmd.instrument.packed});
            m.clientOrderIdField(jt::clientOrderId{cmd.client_order_id.raw});
            m.exchangeClientOrderIdField().assign_range(cmd.exchange_client_order_id.view());
            m.price(jt::priceTicks{cmd.price.raw});
            m.qty(jt::qtyLots{cmd.qty.raw});
            // NOTHING IS WORKING. An order refused before the wire leaves
            // no quantity behind, and a non-zero leaves here would put a
            // phantom in every book that replays this.
            m.leavesQty(jt::qtyLots{0});
            m.tsEvent(jt::timestampNs{clock_->wall().ns});
            m.tsInit(jt::timestampNs{clock_->wall().ns});
        });
        (void)why;
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

// Working lots for ONE strategy on one instrument.
//
// oms::working_lots answers for the whole process, which is the right question
// for the risk gate — the venue sees one book and does not care who filled it
// — and the wrong one for a target, where netting one strategy's orders
// against another's moves a position nobody asked to move.
[[nodiscard]] oms::WorkingLots working_lots_of(const oms::OrderStore& store,
                                               std::uint16_t strategy_id,
                                               InstrumentId instrument) noexcept {
    oms::WorkingLots out{};
    store.for_each_open([&](const oms::Order& order) {
        if (order.instrument() != instrument ||
            order.client_order_id().strategy_id() != strategy_id) {
            return;
        }
        auto& side = order.side() == Side::Buy ? out.buy : out.sell;
        side = QtyLots::from_raw(side.raw + order.leaves().raw);
    });
    return out;
}

// ONE STRATEGY'S WINDOW onto the kernel above.
//
// Everything per-strategy is here — the id mint, the timers, and the slice of
// the book that belongs to this id. Everything shared stays behind it. The
// split is what makes strategy.hpp's promise true once a process runs more
// than one strategy: "the kernel supplies the strategy_id, so a strategy
// cannot read another's book". It cannot cancel another's orders either, and
// that is ENFORCED here rather than assumed.
class InstanceContext final : public strategy::Context {
public:
    // The ULID half of IdGen goes unused here: the exchange-side id is minted
    // once, by the kernel, because what has to be monotone is the process's
    // stream and not one strategy's. The seed is still per instance so that
    // two windows can never agree on a sequence.
    InstanceContext(KernelContext& kernel, std::uint16_t strategy_id, std::uint64_t ulid_seed)
        : kernel_{&kernel}, ids_{strategy_id, ulid_seed} {}

    [[nodiscard]] std::uint16_t strategy_id() const noexcept { return ids_.strategy_id(); }

    std::expected<ClientOrderId, strategy::RejectReason>
    submit(const strategy::NewOrder& order) noexcept override {
        return kernel_->submit_as(order, ids_.next_client_id());
    }

    // ONLY ITS OWN. The id packs its owner, so an id belonging to somebody
    // else is refused rather than cancelled: a strategy that can pull another
    // strategy's quotes is a strategy that can be wrong about more than its
    // own book.
    bool cancel(ClientOrderId id) noexcept override {
        return id.strategy_id() == strategy_id() && kernel_->cancel(id);
    }

    void cancel_all(InstrumentId instrument) noexcept override { (void)pull(&instrument); }

    // Everything this strategy has working, on every instrument. What a stop
    // calls, and the reason a stop can say the market holds none of its orders
    // afterwards.
    std::size_t pull_all() noexcept { return pull(nullptr); }

    std::optional<Bbo> bbo(InstrumentId instrument) const noexcept override {
        return kernel_->bbo(instrument);
    }

    const Instrument* instrument(InstrumentId id) const noexcept override {
        return kernel_->instrument(id);
    }

    const portfolio::Position* position(InstrumentId id) const noexcept override {
        return kernel_->positions.find({strategy_id(), id});
    }

    FixedE9 unrealized(InstrumentId id, PriceTicks mark) const noexcept override {
        const auto* held = position(id);
        const auto* listed = instrument(id);
        if (held == nullptr || listed == nullptr) {
            return FixedE9::from_raw(0);
        }
        return portfolio::unrealized_pnl(base_of(*listed), *held, mark);
    }

    std::optional<ClientOrderId>
    set_target(const portfolio::PortfolioTarget& target) noexcept override {
        auto held = QtyLots::from_raw(0);
        if (const auto* p = position(target.instrument); p != nullptr) {
            held = p->qty();
        }
        const auto working = working_lots_of(kernel_->orders(), strategy_id(), target.instrument);
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

    // TIMERS ARE PER INSTANCE. Two strategies both arming timer 1 arm two
    // timers; one shared table would have each re-arm silently cancel the
    // other's.
    void set_timer(std::uint32_t timer_id, std::uint64_t delay_ns) noexcept override {
        const auto deadline = kernel_->clock().mono().ns + delay_ns;
        for (auto& timer : timers_) {
            if (timer.id == timer_id) {
                timer.deadline_ns = deadline; // re-arm replaces
                return;
            }
        }
        timers_.push_back({timer_id, deadline});
    }

    const Clock& clock() const noexcept override { return kernel_->clock(); }

    // Disarms before it reports, so a strategy re-arming inside the callback
    // sets the next deadline rather than having it cleared out from under it.
    //
    // BY INDEX, over the size as it was on entry: a callback may arm a timer
    // it has never armed before, and a range-for over a vector that grows
    // underneath walks freed memory.
    template <typename F> void fire_due_timers(std::uint64_t now_ns, F&& fire) {
        const auto armed = timers_.size();
        for (std::size_t i = 0; i < armed; ++i) {
            if (timers_[i].deadline_ns == 0 || now_ns < timers_[i].deadline_ns) {
                continue;
            }
            const auto id = timers_[i].id;
            timers_[i].deadline_ns = 0;
            fire(id);
        }
    }

private:
    struct Timer {
        std::uint32_t id{0};
        std::uint64_t deadline_ns{0};
    };

    // `only` names one instrument, or is null for every one of them. Collected
    // before anything is cancelled: `cancel` writes the journal and a ring, and
    // walking the store while doing so asks it to be re-entrant for no reason.
    std::size_t pull(const InstrumentId* only) noexcept {
        std::vector<ClientOrderId> mine;
        kernel_->orders().for_each_open([&](const oms::Order& order) {
            if (order.is_terminal() || order.client_order_id().strategy_id() != strategy_id()) {
                return;
            }
            if (only != nullptr && order.instrument() != *only) {
                return;
            }
            mine.push_back(order.client_order_id());
        });
        std::size_t pulled = 0;
        for (const auto id : mine) {
            pulled += kernel_->cancel(id) ? 1U : 0U;
        }
        return pulled;
    }

    KernelContext* kernel_;
    oms::IdGen ids_;
    std::vector<Timer> timers_;
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

// What one pass over a gateway's catalogue found, for the kernel machine. Not
// counts: the machine only asks whether the venue confirmed a break, and
// whether anything agreed at all — an empty book agrees with nothing and must
// not be read as agreement.
struct ReconSweep {
    bool confirmed_break{false};
    bool any_agreement{false};
};

ReconSweep reconcile_against_venue(KernelContext& ctx,
                                   raikiri::VenueId venue,
                                   const raikiri::shm::CatalogueTable& catalogue,
                                   const raikiri::shm::PositionTable& table,
                                   raikiri::journal::JournalWriter& journal,
                                   raikiri::UnixNanos now) {
    using namespace raikiri;

    ReconSweep sweep;
    std::unordered_map<std::uint64_t, std::int64_t> book;
    for (const auto& held : ctx.positions.all()) {
        book[held.key().instrument.packed] += held.qty().raw;
    }
    // Fills older than any snapshot we would still compare against can never
    // be rewound past again.
    for (auto& [packed, deltas] : ctx.fills_after) {
        oms::prune_fills(deltas, now, k_snapshot_max_age_ns);
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
            local_raw = oms::book_as_of(local_raw, it->second, boundary);
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
            confirmed = ctx.recon_gate.on_break(id, boundary);
            sweep.confirmed_break = sweep.confirmed_break || confirmed;
        } else if (result.verdict == portfolio::ReconVerdict::Agree) {
            ctx.recon_gate.on_agree(id);
            sweep.any_agreement = true;
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
    return sweep;
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
        // NOT the venue's own id for the charge, and NOT the symbol text —
        // oms::funding_identity states why it is neither.
        std::array<char, oms::k_max_funding_identity> buf{};
        m.execIdField().assign_range(
            oms::funding_identity(base.id, charge.settled_at, strategy_id, buf));
        m.amount(jt::fixedE9{amount.raw});
        m.tsEvent(jt::timestampNs{charge.settled_at.ns});
    });
}

// A funding charge, booked onto whoever was carrying the instrument.
//
// The split itself — the pro-rating, the exact-sum remainder, and where a
// charge nobody holds lands — is oms::split_funding, and the reasoning is
// stated there. This walks the book to name the holders and writes the rows.
void journal_funding(journal::JournalWriter& writer,
                     const portfolio::PositionBook& book,
                     const LedgerEntry& charge,
                     const InstrumentBase& base) noexcept {
    std::vector<oms::FundingHolder> holders;
    for (const auto& position : book.all()) {
        if (position.key().instrument == charge.instrument) {
            holders.push_back(
                {.strategy_id = position.key().strategy_id, .lots = position.qty().raw});
        }
    }
    for (const auto& share : oms::split_funding(holders, charge.amount, k_manual_strategy_id)) {
        write_funding_row(writer, charge, base, share.strategy_id, share.amount);
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

    // KEYED ON THE PAIR, not on the two folded into one integer. This was
    // `(strategy_id << 48) ^ instrument.packed`, and a strategy id shifted by
    // 48 lands exactly on the venue and market bits — so strategy 1 holding
    // bybit PERP and the manual book holding bybit SPOT under the same symbol
    // slot produced ONE key, and whichever the journal replayed second erased
    // the other. A pair cannot collide.
    std::map<std::pair<std::uint16_t, std::uint64_t>, Row> latest;
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
        latest[{row.strategy_id, id.packed}] = row;
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

// What one --strategy said. Colons rather than `=` so the whole thing survives
// a shell unquoted, and positional because three optional fields is not a
// syntax worth inventing twice.
struct Requested {
    std::string type;
    std::string name;
    std::string config;
    std::uint16_t strategy_id{0};
};

// `type[:id[:name[:config]]]`. The config is THE WHOLE REMAINDER, colons and
// all, because it is JSON as often as not and this process never reads it.
[[nodiscard]] std::optional<Requested> parse_strategy_spec(std::string_view spec,
                                                           std::uint16_t default_id) {
    Requested out{};
    out.strategy_id = default_id;
    const auto after_type = spec.find(':');
    out.type = std::string{spec.substr(0, after_type)};
    if (out.type.empty()) {
        return std::nullopt;
    }
    out.name = out.type;
    if (after_type == std::string_view::npos) {
        return out;
    }
    const auto rest = spec.substr(after_type + 1);
    const auto after_id = rest.find(':');
    if (const auto text = rest.substr(0, after_id); !text.empty()) {
        std::uint64_t value = 0;
        const auto [stop, ec] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        // A PARTIAL PARSE IS A REFUSAL: "7x" is not the id 7, it is a typo that
        // would otherwise start a strategy under an id nobody chose.
        if (ec != std::errc{} || stop != text.data() + text.size() || value > 0xFFFFULL) {
            return std::nullopt;
        }
        out.strategy_id = static_cast<std::uint16_t>(value);
    }
    if (after_id == std::string_view::npos) {
        return out;
    }
    const auto tail = rest.substr(after_id + 1);
    const auto after_name = tail.find(':');
    if (const auto text = tail.substr(0, after_name); !text.empty()) {
        out.name = std::string{text};
    }
    if (after_name != std::string_view::npos) {
        out.config = std::string{tail.substr(after_name + 1)};
    }
    return out;
}

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
    // 2, NOT 1. One is k_manual_strategy_id, and a strategy holding it would
    // net its position into the manual desk's book — two books that no query
    // could take apart again. The registry refuses it; this is where the
    // default stopped colliding with it.
    std::uint16_t strategy_id = 2;
    // Repeatable. Empty means the demo wiring below, which is itself expressed
    // as a --strategy nobody typed, so there is ONE path from a type name to a
    // running strategy rather than two that can disagree.
    std::vector<std::string> requested;
    // Zero serves no control surface at all: a port bound by default is a port
    // somebody did not decide to open.
    std::uint16_t control_port = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--duration" && i + 1 < argc) {
            duration_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg.starts_with("--venue=") || arg.starts_with("--gateway=")) {
            wanted.emplace_back(arg.substr(arg.find('=') + 1));
        } else if (arg.starts_with("--strategy-id=")) {
            strategy_id = static_cast<std::uint16_t>(std::atoi(arg.substr(14).data()));
        } else if (arg.starts_with("--strategy=")) {
            requested.emplace_back(arg.substr(arg.find('=') + 1));
        } else if (arg == "--strategy" && i + 1 < argc) {
            requested.emplace_back(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            control_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
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
    // The kernel's own mint, used for ONE thing: the exchange-side id, where
    // what must be monotone is this process's stream rather than any single
    // strategy's. Client order ids are minted per instance, by the window that
    // knows whose they are.
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

    // The types this binary carries, put into the process factory by a CALL
    // rather than by a static initializer — see raikiri/strategies/register.hpp
    // for why a static one in a static library is not there when you look.
    (void)strategies::register_all();
    (void)strategy::register_type<Observer>("observer");

    // One window per strategy, minted on demand and NEVER MOVED: a strategy
    // holds this address for its whole life, so a container that reallocates
    // would hand every running strategy a dangling Context.
    std::deque<InstanceContext> contexts;
    // The process's stop signal. Every instance starts holding it, so a
    // shutdown reaches a strategy started over HTTP by the same path it
    // reaches one named on the command line.
    std::stop_source shutdown;
    strategy::Registry live{
        {.context_for =
             [&contexts, &ctx](std::uint16_t id) -> strategy::Context* {
                 for (auto& window : contexts) {
                     if (window.strategy_id() == id) {
                         // A create the constructor then refused already minted
                         // one; handing back the same window keeps a retry
                         // under that id from running beside an orphan.
                         return &window;
                     }
                 }
                 contexts.emplace_back(ctx, id, 0x7A10ULL + id);
                 return &contexts.back();
             },
         .cancel_working =
             [&contexts](std::uint16_t id) -> std::size_t {
                 for (auto& window : contexts) {
                     if (window.strategy_id() == id) {
                         return window.pull_all();
                     }
                 }
                 return 0;
             },
         .wall = [&clock] { return clock.wall(); }}};

    // DemoMaker quotes both sides, which is only ever appropriate against the
    // sim venue. A live gateway is observed, not traded: the loop, the journal
    // and the latency table are identical either way.
    if (requested.empty()) {
        const bool quoting =
            gateways.size() == 1 && gateways.front().account.venue == VenueId::Sim;
        requested.emplace_back(quoting ? "demo-maker" : "observer");
    }
    std::uint16_t next_id = strategy_id;
    for (const auto& spec : requested) {
        const auto parsed = parse_strategy_spec(spec, next_id);
        if (!parsed.has_value()) {
            fmt::print(stderr, "strategyd: cannot read --strategy {}\n", spec);
            return 2;
        }
        const auto made = live.create({.name = parsed->name,
                                       .type = parsed->type,
                                       .config = parsed->config,
                                       .strategy_id = parsed->strategy_id});
        if (!made.has_value()) {
            fmt::print(
                stderr, "strategyd: --strategy {} refused — {}\n", spec, made.error().detail);
            return 2;
        }
        if (const auto started = live.start(parsed->strategy_id, shutdown.get_token());
            !started.has_value()) {
            fmt::print(stderr,
                       "strategyd: --strategy {} would not start — {}\n",
                       spec,
                       to_string(started.error()));
            return 2;
        }
        // The next unnamed one goes BESIDE it rather than on top of it: two
        // instances under one id net into one position.
        next_id = static_cast<std::uint16_t>(parsed->strategy_id + 1);
    }

    std::string attached;
    for (const auto& gw : gateways) {
        attached += (attached.empty() ? "" : ", ") + gw.name;
    }
    std::string running;
    for (const auto& view : live.list()) {
        running += (running.empty() ? "" : ", ") +
                   fmt::format("{}#{} ({})", view.name, view.strategy_id, view.type);
    }
    RLOG_INFO("strategyd: attached to {}, {} instruments, running {} for {}s",
              attached,
              instrument_count,
              running,
              duration_s);

    // LOOPBACK, AND NO AUTHENTICATION — the same position admind takes, for
    // the same reason: shipping an identity story that "works for now" is how
    // a deployment ends up with a static token in a repository. A handler here
    // runs on_start and on_stop, so this server may only ever be polled by the
    // loop below.
    std::unique_ptr<http::Server> control;
    if (control_port != 0) {
        control = std::make_unique<http::Server>(
            "127.0.0.1",
            control_port,
            strategy::make_control(live, strategy::factory(), ctx.positions,
                                   shutdown.get_token()));
        if (!control->start()) {
            RLOG_ERROR("strategyd: cannot bind the control surface on 127.0.0.1:{}", control_port);
            return 1;
        }
        RLOG_INFO("strategyd: control on http://127.0.0.1:{} — admind forwards it with "
                  "--upstream <name>={}",
                  control->port(),
                  control->port());
    }

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

    // AN OWNER THAT HAS NEVER BEATEN IS NOT ONE THAT STOPPED ANSWERING.
    //
    // A segment file exists from the instant it is created — before the owner
    // publishes and before its first heartbeat — and `owner_alive` reads a
    // heartbeat of zero as stale, which is correct for "is it serving?" and
    // wrong as a cause of death. A strategyd started in the same breath as its
    // gateway therefore halted on a gateway that was starting up perfectly
    // well, and with one gateway attached that meant exiting.
    //
    // So wait for the first beat here, where waiting is free, rather than
    // teaching the loop below to tell a birth from a death.
    for (auto& gw : gateways) {
        constexpr int k_tries = 200; // 10 s at 50 ms, the same shape as the attach retry
        int waited = 0;
        while (waited < k_tries && !gw.client.owner_alive(clock.mono().ns)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            ++waited;
        }
        if (waited == k_tries) {
            RLOG_ERROR("strategyd: {} never sent a heartbeat — it is not serving", gw.name);
            return 1;
        }
    }

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
                        // RECORDED, not only logged. Without this the caller
                        // holds an id for an order that exists in no store, no
                        // journal and no database.
                        ctx.journal_refusal(OrderCmd{.kind = CmdKind::Create,
                                                     .instrument = intent.instrument,
                                                     .client_order_id = intent.client_order_id,
                                                     .side = intent.side,
                                                     .order_type = intent.order_type,
                                                     .tif = intent.tif,
                                                     .flags = intent.flags,
                                                     .price = intent.price,
                                                     .qty = intent.qty,
                                                     .ts_created = clock.wall()},
                                            placed.error());
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
                    ++exec_overruns;
                    // A DROPPED RECORD MAY BE A FILL, and the book is what
                    // every later decision is priced off. Stop adding risk on
                    // this gateway until the venue has been asked; cancels
                    // continue, which is the half that matters while blind.
                    if (gw.kernel.on(oms::KernelEvent::ExecOverrun, clock.wall())) {
                        RLOG_WARN("strategyd[{}]: execution record dropped — "
                                  "no new orders until reconciliation agrees",
                                  gw.name);
                    }
                    continue;
                }
                // A COPY: oms::attribute restores our own id onto it, and the
                // ring's record is not ours to write.
                OrderEvent event = rec.event;
                if (const auto it = ctx.inflight_.find(event.client_order_id.raw);
                    it != ctx.inflight_.end()) {
                    rt_hist.record(clock.mono().ns - it->second);
                    ctx.inflight_.erase(it);
                }
                // BEFORE apply_event, which is what moves the leaves this reads.
                // Why it is the drop in leaves and not the event's own qty, and
                // why our id has to be restored first, are stated where they
                // are enforced — see oms/attribution.hpp.
                const auto who = oms::attribute(event, store);
                if (const auto applied = store.apply_event(event);
                    !applied.has_value() && applied.error() == oms::ApplyError::BadTransition) {
                    ++bad_transitions;
                }
                if (event.type == OrderEventType::Filled) {
                    ++fills;
                    if (who.order == nullptr) {
                        // A fill for an order this process never had. Counted
                        // and said out loud: it is money that moved on an
                        // account we hold, and a silent one is the difference
                        // between a quiet desk and a broken one.
                        ++unattributed_fills;
                        RLOG_ERROR("strategyd: FILL {} matched no order here",
                                   event.exchange_client_order_id.view());
                    }
                    const auto delta = oms::filled_delta(who, event);
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
                            ctx.fees.book(event.exec_id.view(), booked_fee, clock.wall());
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
                live.on_order_event(event);
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
                live.on_trade(trade);
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
                    live.on_bbo(bbo);
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
                    live.on_funding(funding);
                }
            }
        }
        const auto now_ns = clock.mono().ns;
        for (auto& window : contexts) {
            window.fire_due_timers(now_ns, [&live, &window](std::uint32_t timer_id) {
                // Through the registry, not straight at the strategy: a timer
                // that fires on an instance being torn down must land nowhere,
                // and the subscription is what knows that.
                live.on_timer(window.strategy_id(), timer_id);
            });
        }
        if (control != nullptr) {
            // Polled here, inside the loop, so a start or a stop arriving over
            // HTTP runs its hooks on the thread that owns the order path.
            (void)control->poll();
        }
        for (auto& gw : gateways) {
            // Monotonic, because that is the clock the gateway stamps the
            // heartbeat with; asking in wall time makes every owner look stale.
            if (!gw.client.owner_alive(now_ns)) {
                // HALTS THIS GATEWAY, not the process. Everything read from a
                // segment belongs to the generation that wrote it, so this
                // book is finished — but a second gateway shares nothing with
                // it and has no reason to stop trading.
                if (gw.kernel.on(oms::KernelEvent::GatewayLost, clock.wall())) {
                    RLOG_ERROR("strategyd[{}]: gateway stopped answering — halted", gw.name);
                }
                continue;
            }
            if (now_ns >= gw.recon_due) {
                // Seven days is the widest window any venue's ledger reaches
                // back over, so a memo older than that will never be quoted.
                ctx.fees.prune(clock.wall(), 7ULL * 24 * 60 * 60 * 1'000'000'000ULL);
                reconcile_open_orders(
                    ctx, gw.account.venue, *gw.ch.open_orders, *journal_writer, clock.wall());
                const auto sweep = reconcile_against_venue(ctx,
                                                           gw.account.venue,
                                                           *gw.ch.catalogue,
                                                           *gw.ch.positions,
                                                           *journal_writer,
                                                           clock.wall());
                // A CONFIRMED BREAK HALTS; agreement only clears a Degraded
                // book, never a halted one. Silence is neither — a sweep that
                // found nothing to compare has not checked anything, and
                // reading it as agreement would let an empty catalogue resume
                // trading.
                if (sweep.confirmed_break) {
                    if (gw.kernel.on(oms::KernelEvent::ReconFailed, clock.wall())) {
                        RLOG_ERROR("strategyd[{}]: venue disagreed — halted until an operator "
                                   "resumes on a book that agrees",
                                   gw.name);
                    }
                } else if (sweep.any_agreement) {
                    if (gw.kernel.on(oms::KernelEvent::ReconAgreed, clock.wall())) {
                        RLOG_INFO("strategyd[{}]: venue agrees — trading again", gw.name);
                    }
                }
                gw.recon_due = now_ns + k_recon_read_interval_ns;
            }
        }
        // Stop only when NOTHING is left to trade on. One halted gateway is a
        // halted book; every gateway halted is a process with no purpose, and
        // spinning on dead segments is worse than exiting.
        if (std::ranges::all_of(gateways, [](const Gateway& gw) {
                return gw.kernel.state() == oms::KernelState::Halted;
            })) {
            gateway_gone = gateways.empty() ? std::string{"none attached"} : gateways.front().name;
            break;
        }
    }
    // ONE TEARDOWN, whatever ended the run — a signal, the duration running
    // out, or every gateway halting. The stop signal reaches each instance
    // through the same ordered path a person's stop takes, so nothing gets a
    // shorter unwind for having been ended by a clock.
    shutdown.request_stop();
    const auto torn_down = live.poll();
    if (control != nullptr) {
        control->stop();
    }
    journal_writer->flush();
    if (!gateway_gone.empty()) {
        RLOG_ERROR("strategyd: every gateway is halted (first: {}) — nothing left to trade on, "
                   "stopping",
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
    // NAMED, not counted. "two strategies stopped" does not say whether the one
    // that was quoting is the one that stopped holding orders.
    fmt::print("strategies: {} stopped at exit\n", torn_down);
    for (const auto& view : live.list()) {
        fmt::print("  {:<16}{:<14}{:<10}orders-pulled={}\n",
                   view.name,
                   view.type,
                   to_string(view.phase),
                   view.orders_cancelled_on_stop);
    }
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
               ctx.recon_gate.halted_count());

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
